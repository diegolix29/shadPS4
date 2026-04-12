// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "nexus_mods_api.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>

static constexpr const char* kBase = "https://api.nexusmods.com/v1";

NexusModsApi::NexusModsApi(QObject* parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)) {}

bool NexusModsApi::autoDetectApiKey() {
    QString env = QProcessEnvironment::systemEnvironment().value("NEXUS_API_KEY");
    if (!env.trimmed().isEmpty()) {
        m_apiKey = env.trimmed();
        return true;
    }

    QStringList dirs = QStandardPaths::standardLocations(QStandardPaths::AppConfigLocation);
    dirs << QDir::homePath() + "/.config/shadPS4";
    for (const QString& d : dirs) {
        QFile f(d + "/nexus_api_key.txt");
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QString k = QTextStream(&f).readLine().trimmed();
            if (!k.isEmpty()) {
                m_apiKey = k;
                return true;
            }
        }
    }

    QFile f2(QDir::homePath() + "/.nexus_api_key");
    if (f2.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString k = QTextStream(&f2).readLine().trimmed();
        if (!k.isEmpty()) {
            m_apiKey = k;
            return true;
        }
    }
    return false;
}

void NexusModsApi::setApiKey(const QString& key) {
    m_apiKey = key.trimmed();
    saveApiKey();
}

void NexusModsApi::saveApiKey() {
    if (m_apiKey.isEmpty()) {
        return;
    }

    QStringList dirs = QStandardPaths::standardLocations(QStandardPaths::AppConfigLocation);
    dirs << QDir::homePath() + "/.config/shadPS4";

    for (const QString& d : dirs) {
        QDir dir;
        if (!dir.mkpath(d)) {
            continue;
        }

        QFile f(d + "/nexus_api_key.txt");
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&f);
            out << m_apiKey;
            f.close();
            return;
        }
    }

    QFile f2(QDir::homePath() + "/.nexus_api_key");
    if (f2.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&f2);
        out << m_apiKey;
        f2.close();
    }
}

void NexusModsApi::validateApiKey() {
    if (m_apiKey.isEmpty()) {
        emit errorOccurred("No API key set.");
        return;
    }
    QNetworkReply* r = m_nam->get(buildRequest("/users/validate.json"));
    connect(r, &QNetworkReply::finished, this, &NexusModsApi::onValidateReply);
}

void NexusModsApi::onValidateReply() {
    auto* r = qobject_cast<QNetworkReply*>(sender());
    if (!r)
        return;
    r->deleteLater();
    if (r->error() != QNetworkReply::NoError) {
        emit apiKeyValidated(false, {});
        return;
    }
    QJsonObject obj = QJsonDocument::fromJson(r->readAll()).object();
    QString name = obj.value("name").toString();
    emit apiKeyValidated(!name.isEmpty(), name);
}

void NexusModsApi::searchMods(const QString& gameSlug, const QString& nameFilter,
                              NexusSearchMode mode) {
    if (m_apiKey.isEmpty()) {
        emit errorOccurred("No Nexus API key.");
        return;
    }
    if (gameSlug.isEmpty()) {
        emit errorOccurred("No game slug for this serial. Try searchAllModes with a known slug.");
        return;
    }

    QString ep;
    switch (mode) {
    case NexusSearchMode::LatestAdded:
        ep = QString("/games/%1/mods/latest_added.json").arg(gameSlug);
        break;
    case NexusSearchMode::LatestUpdated:
        ep = QString("/games/%1/mods/latest_updated.json").arg(gameSlug);
        break;
    case NexusSearchMode::Trending:
        ep = QString("/games/%1/mods/trending.json").arg(gameSlug);
        break;
    }

    QNetworkRequest req = buildRequest(ep);
    req.setAttribute(QNetworkRequest::User, nameFilter);
    req.setAttribute(QNetworkRequest::UserMax, gameSlug);
    QNetworkReply* r = m_nam->get(req);
    connect(r, &QNetworkReply::finished, this, &NexusModsApi::onListingReply);
}

void NexusModsApi::searchAllModes(const QString& gameSlug, const QString& nameFilter) {
    if (m_apiKey.isEmpty()) {
        emit errorOccurred("No Nexus API key.");
        return;
    }
    if (gameSlug.isEmpty()) {
        emit errorOccurred("Game not found in Nexus game list for this serial.");
        return;
    }

    m_pendingFilter = nameFilter;
    m_pendingSlug = gameSlug;
    m_pendingReplies = 3;
    m_mergedResults.clear();

    const QStringList endpoints = {
        QString("/games/%1/mods/latest_added.json").arg(gameSlug),
        QString("/games/%1/mods/latest_updated.json").arg(gameSlug),
        QString("/games/%1/mods/trending.json").arg(gameSlug),
    };

    for (const QString& ep : endpoints) {
        QNetworkRequest req = buildRequest(ep);
        req.setAttribute(QNetworkRequest::User, nameFilter);
        req.setAttribute(QNetworkRequest::UserMax, gameSlug);
        QNetworkReply* r = m_nam->get(req);
        r->setProperty("merged", true);
        connect(r, &QNetworkReply::finished, this, &NexusModsApi::onListingReply);
    }
}

void NexusModsApi::onListingReply() {
    auto* r = qobject_cast<QNetworkReply*>(sender());
    if (!r)
        return;
    r->deleteLater();

    bool isMerged = r->property("merged").toBool();
    QString filter = r->request().attribute(QNetworkRequest::User).toString();
    QString slug = r->request().attribute(QNetworkRequest::UserMax).toString();

    if (r->error() != QNetworkReply::NoError) {
        if (isMerged) {
            if (--m_pendingReplies == 0)
                finalizeMergedResults();
        } else {
            handleNetworkError(r, "listing");
        }
        return;
    }

    QList<NexusModResult> batch = parseModArray(r->readAll(), slug);

    if (isMerged) {
        for (const NexusModResult& m : batch)
            m_mergedResults.append(m);

        if (--m_pendingReplies == 0)
            finalizeMergedResults();
    } else {
        QList<NexusModResult> filtered;
        QString lf = filter.toLower().normalized(QString::NormalizationForm_C);
        for (const NexusModResult& m : batch) {
            QString normalizedName = m.name.toLower().normalized(QString::NormalizationForm_C);
            QString normalizedSummary =
                m.summary.toLower().normalized(QString::NormalizationForm_C);
            if (lf.isEmpty() || normalizedName.contains(lf) || normalizedSummary.contains(lf))
                filtered.append(m);
        }
        emit searchResultsReady(filtered, filtered.size());
    }
}

void NexusModsApi::finalizeMergedResults() {
    QSet<int> seen;
    QList<NexusModResult> unique;
    for (const NexusModResult& m : m_mergedResults) {
        if (!seen.contains(m.modId)) {
            seen.insert(m.modId);
            unique.append(m);
        }
    }

    QString lf = m_pendingFilter.toLower().normalized(QString::NormalizationForm_KD);
    QList<NexusModResult> filtered;
    for (const NexusModResult& m : unique) {
        QString normalizedName = m.name.toLower().normalized(QString::NormalizationForm_KD);
        QString normalizedSummary = m.summary.toLower().normalized(QString::NormalizationForm_KD);
        if (lf.isEmpty() || normalizedName.contains(lf) || normalizedSummary.contains(lf))
            filtered.append(m);
    }

    emit searchResultsReady(filtered, filtered.size());
}

QList<NexusModResult> NexusModsApi::parseModArray(const QByteArray& data,
                                                  const QString& gameSlug) const {
    QList<NexusModResult> results;
    QJsonDocument doc = QJsonDocument::fromJson(data);

    QJsonArray arr;
    if (doc.isArray())
        arr = doc.array();
    else if (doc.isObject())
        arr = doc.object().value("data").toArray();

    for (const QJsonValue& v : arr) {
        QJsonObject o = v.toObject();
        NexusModResult r;
        r.modId = o.value("mod_id").toInt();
        r.name = o.value("name").toString();
        r.summary = o.value("summary").toString();
        r.author = o.value("author").toString().isEmpty()
                       ? o.value("user").toObject().value("name").toString()
                       : o.value("author").toString();
        r.pictureUrl = o.value("picture_url").toString();
        r.gameSlug = gameSlug;
        r.categoryName = o.value("category_name").toString();
        r.downloadCount = o.value("mod_downloads").toInt();
        r.endorsementCount = o.value("endorsement_count").toInt();
        r.version = o.value("version").toString();
        r.adult = o.value("contains_adult_content").toBool();
        if (r.modId > 0 && !r.name.isEmpty())
            results.append(r);
    }
    return results;
}

void NexusModsApi::fetchModFiles(const QString& gameSlug, int modId) {
    if (m_apiKey.isEmpty()) {
        emit errorOccurred("No Nexus API key.");
        return;
    }
    QString ep = QString("/games/%1/mods/%2/files.json").arg(gameSlug).arg(modId);
    QNetworkReply* r = m_nam->get(buildRequest(ep));
    r->setProperty("modId", modId);
    connect(r, &QNetworkReply::finished, this, &NexusModsApi::onModFilesReply);
}

void NexusModsApi::onModFilesReply() {
    auto* r = qobject_cast<QNetworkReply*>(sender());
    if (!r)
        return;
    r->deleteLater();
    int modId = r->property("modId").toInt();
    if (r->error() != QNetworkReply::NoError) {
        handleNetworkError(r, "files");
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(r->readAll());
    QJsonArray arr = doc.isObject() ? doc.object().value("files").toArray() : doc.array();

    QList<NexusFileInfo> files;
    for (const QJsonValue& v : arr) {
        QJsonObject o = v.toObject();
        NexusFileInfo fi;
        fi.fileId = o.value("file_id").toInt();
        fi.name = o.value("name").toString();
        fi.version = o.value("version").toString();
        fi.description = o.value("description").toString();
        fi.sizeKb = static_cast<qint64>(o.value("size_kb").toDouble());
        fi.category = o.value("category_name").toString();
        files.append(fi);
    }
    emit modFilesReady(modId, files);
}

void NexusModsApi::fetchDownloadLink(const QString& gameSlug, int modId, int fileId) {
    if (m_apiKey.isEmpty()) {
        emit errorOccurred("No Nexus API key.");
        return;
    }
    m_pendingModId = modId;
    m_pendingFileId = fileId;
    QString ep = QString("/games/%1/mods/%2/files/%3/download_link.json")
                     .arg(gameSlug)
                     .arg(modId)
                     .arg(fileId);
    QNetworkReply* r = m_nam->get(buildRequest(ep));
    connect(r, &QNetworkReply::finished, this, &NexusModsApi::onDownloadLinkReply);
}

void NexusModsApi::onDownloadLinkReply() {
    auto* r = qobject_cast<QNetworkReply*>(sender());
    if (!r)
        return;
    r->deleteLater();
    if (r->error() != QNetworkReply::NoError) {
        handleNetworkError(r, "download link");
        return;
    }

    QJsonArray arr = QJsonDocument::fromJson(r->readAll()).array();
    if (!arr.isEmpty()) {
        emit downloadLinkReady(m_pendingModId, m_pendingFileId,
                               arr.first().toObject().value("URI").toString());
    } else {
        emit errorOccurred("No download link returned by Nexus.");
    }
}

QNetworkRequest NexusModsApi::buildRequest(const QString& endpoint) const {
    QNetworkRequest req(QUrl(QString("%1%2").arg(kBase, endpoint)));
    req.setRawHeader("apikey", m_apiKey.toUtf8());
    req.setRawHeader("Accept", "application/json");
    return req;
}

void NexusModsApi::handleNetworkError(QNetworkReply* r, const QString& ctx) {
    int code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString msg;
    if (code == 401)
        msg = "Invalid or expired Nexus API key.";
    else if (code == 404)
        msg = QString("Nexus %1: game or mod not found (404).").arg(ctx);
    else if (code == 429)
        msg = "Nexus rate-limit reached. Please wait a moment.";
    else
        msg = QString("Nexus %1 failed: %2").arg(ctx, r->errorString());
    emit errorOccurred(msg);
}

QString NexusModsApi::gameSlugForSerial(const QString& serial) {
    static const QHash<QString, QString> map = {
        {"CUSA03173", "bloodborne"},
        {"CUSA00207", "bloodborne"},
        {"CUSA00208", "bloodborne"},
        {"CUSA00900", "bloodborne"},
        {"CUSA00299", "bloodborne"},
        {"CUSA07408", "godofwar"},
        {"CUSA11100", "godofwar"},
        {"CUSA17416", "persona5royal"},
        {"CUSA24769", "persona5royal"},
        {"CUSA28898", "eldenring"},
        {"CUSA08495", "darksoulsremastered"},
        {"CUSA28863", "eldenring"},
        {"CUSA34574", "eldenring"},
        {"CUSA09737", "darksoulsremastered"},
        {"CUSA18543", "ghostoftsushima"},
        {"CUSA13789", "sekiro"},
        {"CUSA13801", "sekiro"},
        {"CUSA50617", "eldenringnightreign"},
        {"CUSA01760", "darksouls2"},
        {"CUSA03388", "darksouls3"},
        {"CUSA17161", "deathstranding"},
        {"CUSA07319", "horizonzerodawn"},
        {"CUSA00552", "thelastofus"},
        {"CUSA10249", "detroitbecomehuman"},
        {"CUSA11395", "marvelsspiderman"},
        {"CUSA04551", "nierautomata"},
        {"CUSA04480", "nierautomata"},
        {"CUSA16760", "finalfantasy7remake"},
        {"CUSA09564", "demonsouls"},
        {"CUSA08617", "shadowofthecolossus"},
        {"CUSA01073", "ratchetandclankps4"},
    };
    return map.value(serial.toUpper(), QString());
}