"""Generates signed/invalid test profiles for the Motiva performance-intelligence
mock backend. DEV ONLY: uses the placeholder key in dev_private_key.pem."""
import base64, json, sys, time, pathlib
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

HERE = pathlib.Path(__file__).parent
KEY = serialization.load_pem_private_key(
    b"".join(l for l in (HERE / "dev_private_key.pem").read_bytes().splitlines(True) if not l.startswith(b"#")), None)

def sign(payload: bytes, key=KEY) -> str:
    r, s = decode_dss_signature(key.sign(payload, ec.ECDSA(hashes.SHA256())))
    return base64.b64encode(r.to_bytes(32, "big") + s.to_bytes(32, "big")).decode()

def envelope(payload: dict, key=KEY, tamper=False, unsigned=False) -> str:
    raw = json.dumps(payload, separators=(",", ":")).encode()
    sig = "" if unsigned else sign(raw, key)
    if tamper:  # change the payload after signing
        raw = raw.replace(b'"version":', b'"version": ')
    return json.dumps({"payload": base64.b64encode(raw).decode(), "signature": sig})

def payload(version=1, params=None, issued=None, expires=None, vendor="any"):
    now = int(time.time())
    return {"schema": 1, "version": version, "issuedAt": issued or now - 60, "expiresAt": expires or now + 86400 * 30,
            "rules": [{"match": {"gpuVendor": vendor, "minRamGb": 0}, "params": params or {"verifyWaitMs": 500}}]}

def make(outdir):
    out = pathlib.Path(outdir); out.mkdir(parents=True, exist_ok=True)
    other = ec.generate_private_key(ec.SECP256R1())
    now = int(time.time())
    files = {
        "valid_v1.json": envelope(payload(1, {"verifyWaitMs": 500, "attachRetryMs": 200})),
        "valid_v2.json": envelope(payload(2, {"verifyWaitMs": 600})),
        "valid_v3.json": envelope(payload(3, {"verifyWaitMs": 650})),
        "unsigned.json": envelope(payload(3), unsigned=True),
        "wrong_key.json": envelope(payload(4), key=other),
        "tampered.json": envelope(payload(5), tamper=True),
        "expired.json": envelope(payload(6, issued=now - 7200, expires=now - 3600)),
        "out_of_range.json": envelope(payload(7, {"verifyWaitMs": 5})),
        "unknown_param.json": envelope(payload(8, {"verifyWaitMs": 500, "killProcess": 1})),
        "garbage.json": "not json at all",
    }
    for name, text in files.items():
        (out / name).write_text(text)
    return out

if __name__ == "__main__":
    make(sys.argv[1])
