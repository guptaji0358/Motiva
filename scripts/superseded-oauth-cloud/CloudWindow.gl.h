#pragma once

#include "CloudProvider.h"
#include "GlWindow.h"

#include <QPair>
#include <QStandardItemModel>

#include <functional>

class CloudManager;

// Connect OneDrive / Google Drive / Dropbox, browse a drive, tick files and
// add them to a playlist. Ticked files are downloaded into Motiva's local
// cache first; the playlist then references those local copies, so rendering
// never depends on the network and keeps working offline.
class CloudWindow : public GlWindow {
    Q_OBJECT
public:
    explicit CloudWindow(QWidget* parent = nullptr);

    // Which playlist the files are for and which file names it can hold.
    void setTarget(const QString& playlistName, std::function<bool(const QString& fileName)> accepts);
    void present();
    void layoutContent(const QRectF& content, const QRectF& footer) override;

signals:
    // Local, cached copies ready to be added to the playlist.
    void filesReady(const QStringList& localPaths);

private:
    QString providerId() const;
    void refreshAccount();
    void openFolder(const QString& folderId, const QString& name, bool push);
    void goUp();
    void reload();
    void loadPage(const QString& pageToken, int generation, int pages);
    void toggleRow(int row);
    void updateSelection();
    void setupAccount();
    void downloadSelected();

    CloudManager* m_cloud;
    std::function<bool(const QString&)> m_accepts;
    QString m_playlistName;

    GlDropdown* m_provider = nullptr;
    GlLabel* m_status = nullptr;
    GlButton* m_setup = nullptr;
    GlButton* m_connect = nullptr;
    GlLabel* m_location = nullptr;
    GlButton* m_up = nullptr;
    GlButton* m_refresh = nullptr;
    GlItemView* m_list = nullptr;
    GlLabel* m_info = nullptr;
    GlProgressBar* m_progress = nullptr;
    GlButton* m_selectAll = nullptr;
    GlButton* m_close = nullptr;
    GlButton* m_add = nullptr;
    QStandardItemModel* m_model = nullptr;

    // Folder trail for the current provider: (folder id, display name).
    QList<QPair<QString, QString>> m_trail;
    QList<CloudEntry> m_entries; // parallel to m_model rows
    int m_listGeneration = 0;
    bool m_loading = false;
};
