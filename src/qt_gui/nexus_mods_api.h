// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

enum class NexusSearchMode {
    LatestAdded,
    LatestUpdated,
    Trending,
};

struct NexusModResult {
    int modId{0};
    QString name;
    QString summary;
    QString author;
    QString pictureUrl;
    QString gameSlug;
    QString categoryName;
    int downloadCount{0};
    int endorsementCount{0};
    QString version;
    bool adult{false};
};

struct NexusFileInfo {
    int fileId{0};
    QString name;
    QString version;
    QString description;
    qint64 sizeKb{0};
    QString category;
};

class NexusModsApi : public QObject {
    Q_OBJECT

public:
    explicit NexusModsApi(QObject* parent = nullptr);

    bool autoDetectApiKey();
    void setApiKey(const QString& key);
    QString apiKey() const {
        return m_apiKey;
    }
    bool hasApiKey() const {
        return !m_apiKey.isEmpty();
    }
    void validateApiKey();
    void saveApiKey();

    void searchMods(const QString& gameSlug, const QString& nameFilter,
                    NexusSearchMode mode = NexusSearchMode::LatestAdded);

    void searchAllModes(const QString& gameSlug, const QString& nameFilter);

    void fetchModFiles(const QString& gameSlug, int modId);
    void fetchDownloadLink(const QString& gameSlug, int modId, int fileId);

    static QString gameSlugForSerial(const QString& serial);

signals:
    void apiKeyValidated(bool valid, const QString& username);
    void searchResultsReady(const QList<NexusModResult>& results, int totalCount);
    void modFilesReady(int modId, const QList<NexusFileInfo>& files);
    void downloadLinkReady(int modId, int fileId, const QString& url);
    void errorOccurred(const QString& message);

private slots:
    void onValidateReply();
    void onListingReply();
    void onModFilesReply();
    void onDownloadLinkReply();

private:
    QNetworkRequest buildRequest(const QString& endpoint) const;
    void handleNetworkError(QNetworkReply* reply, const QString& context);
    void finalizeMergedResults();
    QList<NexusModResult> parseModArray(const QByteArray& data, const QString& gameSlug) const;

    QNetworkAccessManager* m_nam{nullptr};
    QString m_apiKey;

    QString m_pendingFilter;
    QString m_pendingSlug;
    int m_pendingReplies{0};
    QList<NexusModResult> m_mergedResults;

    int m_pendingModId{0};
    int m_pendingFileId{0};
};