"""Per-target skip_build freshness for deploy_local.py (read-only: never runs ninja/cmake, never writes).

Dependencies come from the generated out/build/build.ninja (parsed as text), not from guesses:
  * each artifact's own transitive graph (explicit + implicit inputs; order-only inputs are followed only to reach
    generated sources such as moc/rcc output, never to another linked binary: add_dependencies is ordering, not content);
  * header/source directories the graph cannot list (moc/compile header deps live in the binary .ninja_deps): every
    project-owned -I directory and the directory of each project source of THAT target is swept;
  * shared build configuration for all targets: root CMakeLists.txt, CMakeCache.txt, build.ninja and the project-owned
    inputs of the build.ninja regeneration rule.
Any doubt (unreadable/odd build.ninja, no edge for an artifact, a missing input, no project source) => that target is
NOT fresh and the reason says why. An input as new as the artifact counts as newer.
"""
import os
import re

CODE_EXT = (".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hh", ".inl", ".tpp", ".ui", ".qrc", ".rc", ".manifest",
            ".cmake", ".in", ".qss")
SRC_EXT = (".cpp", ".cc", ".cxx", ".c", ".rc")
BINARY_EXT = (".exe", ".dll", ".a", ".lib", ".so")
_SPLIT = re.compile(r"(?<!\$) +")
_COLON = re.compile(r"(?<!\$):")


class Ambiguous(Exception):
    pass


def _unescape(tok, top):
    def var(m):
        if m.group(1) not in top:
            raise Ambiguous("build.ninja uses unknown variable ${%s}" % m.group(1))
        return top[m.group(1)]
    tok = re.sub(r"\$\{(\w+)\}", var, tok)
    return re.sub(r"\$([ :$])", r"\1", tok)


def parse_ninja(path):
    """-> {output_abs: {"in": [...], "impl": [...], "order": [...], "vars": {...}, "phony": bool}} (abs normcased)."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            raw = fh.read().replace("\r\n", "\n")
    except OSError as ex:
        raise Ambiguous("cannot read %s: %s" % (path, ex))
    lines, buf = [], ""
    for ln in raw.split("\n"):
        if ln.endswith("$") and not ln.endswith("$$"):
            buf += ln[:-1]
            continue
        lines.append(buf + ln)
        buf = ""
    base = os.path.dirname(os.path.abspath(path))

    def norm(p):
        return os.path.normcase(os.path.normpath(os.path.join(base, p)))

    top, edges, cur = {}, {}, None
    for ln in lines:
        if not ln.strip() or ln.lstrip().startswith("#"):
            continue
        if ln[0] in " \t":
            m = re.match(r"\s+(\w+)\s*=\s?(.*)$", ln)
            if m and cur is not None:
                cur["vars"][m.group(1)] = m.group(2)
            continue
        cur = None
        if ln.startswith("subninja"):
            raise Ambiguous("build.ninja uses subninja (multi-config); dependencies cannot be established")
        if ln.startswith("build "):
            body = ln[6:]
            m = _COLON.search(body)
            if not m:
                raise Ambiguous("unparseable build line: %s" % ln[:120])
            left, right = body[:m.start()], body[m.end():]
            outs = [t for t in _SPLIT.split(left.strip()) if t]
            toks = [t for t in _SPLIT.split(right.strip()) if t]
            if not outs or not toks:
                raise Ambiguous("unparseable build line: %s" % ln[:120])
            ins, impl, order, mode = [], [], [], 0
            for t in toks[1:]:
                if t == "|":
                    mode = 1
                elif t == "||":
                    mode = 2
                else:
                    (ins, impl, order)[mode].append(norm(_unescape(t, top)))
            real = []
            for t in outs:  # implicit outputs (after '|') are still outputs of this edge
                if t == "|":
                    continue
                real.append(norm(_unescape(t, top)))
            edge = {"in": ins, "impl": impl, "order": order, "vars": {}, "phony": toks[0] == "phony"}
            for o in real:
                edges[o] = edge
            cur = edge
            continue
        m = re.match(r"(\w+)\s*=\s?(.*)$", ln)
        if m:
            top[m.group(1)] = m.group(2)
    if not edges:
        raise Ambiguous("build.ninja has no build edges")
    return edges


def _closure(edges, target):
    """(leaves, edges_used). Order-only links never lead to another linked binary."""
    leaves, used, seen, stack = set(), [], set(), [(target, False)]
    while stack:
        node, weak = stack.pop()
        if (node, weak) in seen:
            continue
        seen.add((node, weak))
        if weak and node.endswith(BINARY_EXT):
            continue
        e = edges.get(node)
        if e is None:
            leaves.add(node)
            continue
        used.append(e)
        for n in e["in"] + e["impl"]:
            stack.append((n, weak))
        for n in e["order"]:
            stack.append((n, True))
    return leaves, used


def _under(p, d):
    d = os.path.normcase(os.path.normpath(d))
    return p == d or p.startswith(d + os.sep)


def _include_dirs(e_vars, build_dir):
    out = []
    for m in re.finditer(r"(?:^|\s)-(?:I|isystem)\s*(\"[^\"]+\"|\S+)", e_vars.get("INCLUDES", "")):
        out.append(os.path.normcase(os.path.normpath(os.path.join(build_dir, m.group(1).strip('"')))))
    return out


def _sweep(d, found):
    for dp, _dn, files in os.walk(d):
        for f in files:
            if f.lower().endswith(CODE_EXT):
                found.add(os.path.normcase(os.path.normpath(os.path.join(dp, f))))


def _newest(paths):
    best, bp, n = -1, None, 0
    for p in paths:
        try:
            m = os.stat(p).st_mtime_ns
        except OSError:
            continue
        n += 1
        if m > best:
            best, bp = m, p
    return best, bp, n


def _target_state(edges, art, root, build_dir, shared, out_root):
    name = os.path.basename(art)
    if not os.path.isfile(art):
        return {"fresh": False, "reason": "build artifact missing: %s" % art}
    key = os.path.normcase(os.path.normpath(art))
    if key not in edges:
        return {"fresh": False, "ambiguous": True,
                "reason": "%s has no build rule in build.ninja; dependencies cannot be established" % name}
    leaves, used = _closure(edges, key)
    nroot = os.path.normcase(os.path.normpath(root))
    files, missing, sources, sweep = set(shared), [], [], set()
    for lf in leaves:
        if os.path.isfile(lf):
            files.add(lf)
            if _under(lf, nroot) and lf.endswith(SRC_EXT):
                sources.append(lf)
        elif not os.path.isdir(lf):
            missing.append(lf)
    if missing:
        return {"fresh": False, "ambiguous": True,
                "reason": "%s: %d build input(s) missing (e.g. %s); reconfigure/build instead of skip_build"
                          % (name, len(missing), sorted(missing)[0])}
    if not sources:
        return {"fresh": False, "ambiguous": True,
                "reason": "%s: no project source files found in its dependency graph; freshness cannot be "
                          "established" % name}
    for e in used:
        for d in _include_dirs(e["vars"], build_dir):
            if _under(d, nroot) and not _under(d, out_root) and os.path.isdir(d):
                sweep.add(d)
    for s in sources:
        sweep.add(os.path.dirname(s))
    for d in sweep:
        _sweep(d, files)
    oldest = os.stat(art).st_mtime_ns
    newest, npath, count = _newest(files)
    if not npath:
        return {"fresh": False, "ambiguous": True, "reason": "%s: no readable inputs; freshness cannot be established" % name}
    info = {"inputs_checked": count, "newest_input": npath}
    if newest >= oldest:
        info.update(fresh=False, reason="%s: input newer than (or as new as) the artifact: %s" % (name, npath))
    else:
        info.update(fresh=True, reason="%s: all %d inputs older than the artifact" % (name, count))
    return info


def freshness(root, build_dir, artifacts):
    """{"fresh": bool, "reason": str, "targets": {exe: {...}}}. Each artifact is judged only against its own inputs."""
    try:
        edges = parse_ninja(os.path.join(build_dir, "build.ninja"))
        shared = set()
        cfg = [os.path.join(root, "CMakeLists.txt"), os.path.join(build_dir, "CMakeCache.txt"),
               os.path.join(build_dir, "build.ninja")]
        nroot = os.path.normcase(os.path.normpath(root))
        for p in cfg:
            if not os.path.isfile(p):
                raise Ambiguous("shared build configuration missing: %s" % p)
            shared.add(os.path.normcase(os.path.normpath(p)))
        regen = edges.get(os.path.normcase(os.path.normpath(os.path.join(build_dir, "build.ninja"))))
        if regen:  # project-owned CMake inputs (sub-CMakeLists, project .cmake files)
            for p in regen["in"] + regen["impl"]:
                if _under(p, nroot) and os.path.isfile(p):
                    shared.add(p)
    except Ambiguous as ex:
        return {"fresh": False, "reason": "dependency information unreliable: %s" % ex, "targets": {}}
    out_root = os.path.join(root, "out")
    targets = {}
    for a in artifacts:
        try:
            targets[os.path.basename(a)] = _target_state(edges, a, root, build_dir, shared, out_root)
        except (Ambiguous, OSError) as ex:
            targets[os.path.basename(a)] = {"fresh": False, "ambiguous": True,
                                            "reason": "%s: %s" % (os.path.basename(a), ex)}
    bad = [t["reason"] for t in targets.values() if not t["fresh"]]
    if bad:
        return {"fresh": False, "reason": "; ".join(bad), "targets": targets}
    return {"fresh": True, "reason": "; ".join(t["reason"] for t in targets.values()), "targets": targets}
