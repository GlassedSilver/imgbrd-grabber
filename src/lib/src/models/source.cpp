#include "models/source.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QJSValue>
#include <QJSValueIterator>
#include <QMutex>
#include "auth/auth-const-field.h"
#include "auth/auth-field.h"
#include "auth/auth-hash-field.h"
#include "auth/http-auth.h"
#include "auth/http-basic-auth.h"
#include "auth/oauth1-auth.h"
#include "auth/oauth2-auth.h"
#include "auth/url-auth.h"
#include "functions.h"
#include "logger.h"
#include "models/api/api.h"
#include "models/api/javascript-api.h"
#include "models/site.h"
#include "js-helpers.h"


QString getUpdaterBaseUrl()
{
	#if defined NIGHTLY || defined QT_DEBUG
		return QStringLiteral("https://raw.githubusercontent.com/Bionus/imgbrd-grabber/develop/src/sites");
	#else
		return QStringLiteral("https://raw.githubusercontent.com/Bionus/imgbrd-grabber/master/src/sites");
	#endif
}

QJSEngine *Source::jsEngine()
{
	static QJSEngine *engine = nullptr;

	if (engine == nullptr) {
		engine = buildJsEngine(m_dir.readPath("../helper.js"));
	}

	return engine;
}
QMutex *Source::jsEngineMutex()
{
	static QMutex *mutex = nullptr;

	if (mutex == nullptr) {
		mutex = new QMutex();
	}

	return mutex;
}

Source::Source(Profile *profile, const ReadWritePath &dir)
	: m_dir(dir), m_diskName(QFileInfo(dir.readPath()).fileName()), m_profile(profile), m_updater(m_diskName, m_dir, getUpdaterBaseUrl())
{
	// Tag format mapper
	static const QMap<QString, TagNameFormat::CaseFormat> caseAssoc
	{
		{ "lower", TagNameFormat::Lower },
		{ "upper_first", TagNameFormat::UpperFirst },
		{ "upper", TagNameFormat::Upper },
		{ "caps", TagNameFormat::Caps },
	};

	// Javascript models
	QFile js(m_dir.readPath("model.js"));
	if (js.exists() && js.open(QIODevice::ReadOnly | QIODevice::Text)) {
		log(QStringLiteral("Using Javascript model for %1").arg(m_diskName), Logger::Debug);

		const QString src = "(function() { var window = {}; " + js.readAll().replace("export var source = ", "return ") + " })()";

		auto *engine = jsEngine();
		m_jsSource = engine->evaluate(src, js.fileName());
		if (m_jsSource.isError()) {
			log(QStringLiteral("Uncaught exception at line %1: %2").arg(m_jsSource.property("lineNumber").toInt()).arg(m_jsSource.toString()), Logger::Error);
		} else {
			m_name = m_jsSource.property("name").toString();
			m_additionalTokens = jsToStringList(m_jsSource.property("tokens"));

			// Get the list of APIs for this Source
			const QJSValue apis = m_jsSource.property("apis");
			QJSValueIterator it(apis);
			while (it.hasNext()) {
				it.next();
				m_apis.append(new JavascriptApi(engine, m_jsSource, jsEngineMutex(), it.name()));
			}
			if (m_apis.isEmpty()) {
				log(QStringLiteral("No valid source has been found in the model.js file from %1.").arg(m_name));
			}

			// Read tag naming format
			const QJSValue &tagFormat = m_jsSource.property("tagFormat");
			if (!tagFormat.isUndefined()) {
				const auto caseFormat = caseAssoc.value(tagFormat.property("case").toString(), TagNameFormat::Lower);
				m_tagNameFormat = TagNameFormat(caseFormat, tagFormat.property("wordSeparator").toString());
			}

			// Read auth information
			const QJSValue auths = m_jsSource.property("auth");
			QJSValueIterator authIt(auths);
			while (authIt.hasNext()) {
				authIt.next();

				const QString &id = authIt.name();
				const QJSValue &auth = authIt.value();

				const QString type = auth.property("type").toString();
				Auth *ret = nullptr;

				const QJSValue check = auth.property("check");
				const QString checkType = check.isObject() ? check.property("type").toString() : QString();

				if (type == "oauth2") {
					ret = new OAuth2Auth(type, auth);
				} else if (type == "oauth1") {
					ret = new OAuth1Auth(type, auth);
				} else if (type == "http_basic") {
					const int maxPage = checkType == "max_page" ? check.property("value").toInt() : 0;
					const QString passwordType = auth.property("passwordType").toString();
					ret = new HttpBasicAuth(type, maxPage, passwordType);
				} else {
					QList<AuthField*> fields;
					const QJSValue &jsFields = auth.property("fields");
					const quint32 length = jsFields.property("length").toUInt();
					for (quint32 i = 0; i < length; ++i) {
						const QJSValue &field = jsFields.property(i);

						const QString fid = !field.property("id").isUndefined() ? field.property("id").toString() : QString();
						const QString key = !field.property("key").isUndefined() ? field.property("key").toString() : QString();
						const QString type = field.property("type").toString();

						if (type == "hash") {
							const QString algoStr = field.property("hash").toString();
							const auto algo = algoStr == "sha1" ? QCryptographicHash::Sha1 : QCryptographicHash::Md5;
							fields.append(new AuthHashField(key, algo, field.property("salt").toString()));
						} else if (type == "const") {
							const QString value = field.property("value").toString();
							fields.append(new AuthConstField(key, value));
						} else {
							const QString def = !field.property("def").isUndefined() ? field.property("def").toString() : QString();
							fields.append(new AuthField(fid, key, type == "password" ? AuthField::Password : AuthField::Text, def));
						}
					}

					if (type == "get" || type == "post") {
						const QString url = auth.property("url").toString();
						const QString cookie = checkType == "cookie" ? check.property("key").toString() : QString();
						const QString redirectUrl = checkType == "redirect" ? check.property("url").toString() : QString();

						const QJSValue &csrf = auth.property("csrf");
						const QString csrfUrl = csrf.isObject() ? csrf.property("url").toString() : QString();
						const QStringList csrfFields = csrf.isObject() ? jsToStringList(csrf.property("fields")) : QStringList();

						ret = new HttpAuth(type, url, fields, cookie, redirectUrl, csrfUrl, csrfFields);
					} else {
						const int maxPage = checkType == "max_page" ? check.property("value").toInt() : 0;
						ret = new UrlAuth(type, fields, maxPage);
					}
				}

				if (ret != nullptr) {
					m_auths.insert(id, ret);
				}
			}
		}

		js.close();
	} else {
		log(QStringLiteral("Javascript model not found for '%1' in '%2'").arg(m_diskName, js.fileName()), Logger::Warning);
	}

	// Get the list of all sites pertaining to this source
	QFile f(m_dir.readPath("sites.txt"));
	if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
		while (!f.atEnd()) {
			QString line = f.readLine().trimmed();
			if (line.isEmpty()) {
				continue;
			}

			auto site = new Site(line, this);
			m_sites.append(site);
		}
	}
	if (m_sites.isEmpty()) {
		log(QStringLiteral("No site for source %1").arg(m_name), Logger::Debug);
	}

	// Get the list of all supported sites for this source
	QFile supportedFile(m_dir.readPath("supported.txt"));
	if (supportedFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
		while (!supportedFile.atEnd()) {
			const QString line = supportedFile.readLine().trimmed();
			if (line.isEmpty()) {
				continue;
			}
			m_supportedSites.append(line);
		}
	}
}

Source::~Source()
{
	qDeleteAll(m_apis);
	qDeleteAll(m_sites);
	qDeleteAll(m_auths);
}


bool Source::addSite(Site *site)
{
	// Read current sites
	QFile read(m_dir.readPath("sites.txt"));
	if (!read.open(QIODevice::ReadOnly)) {
		return false;
	}
	QString rawSites = read.readAll();
	read.close();

	// Add site to data
	QStringList sites = rawSites.replace("\r", "").split("\n", Qt::SkipEmptyParts);
	sites.append(site->url());
	sites.removeDuplicates();
	sites.sort();

	// Save new sites
	return writeFile(m_dir.writePath("sites.txt"), sites.join("\r\n").toLatin1());
}

bool Source::removeSite(Site *site)
{
	// Read current sites
	QFile read(m_dir.readPath("sites.txt"));
	if (!read.open(QIODevice::ReadOnly)) {
		return false;
	}
	QString rawSites = read.readAll();
	read.close();

	// Remove the site from the list
	QStringList sites = rawSites.replace("\r", "").split("\n", Qt::SkipEmptyParts);
	sites.removeAll(site->url());

	// Save new sites
	return writeFile(m_dir.writePath("sites.txt"), sites.join("\r\n").toLatin1());
}


QString Source::getName() const { return m_name; }
ReadWritePath Source::getPath() const { return m_dir; }
const QList<Site*> &Source::getSites() const { return m_sites; }
const QStringList &Source::getSupportedSites() const { return m_supportedSites; }
const QList<Api*> &Source::getApis() const { return m_apis; }
Profile *Source::getProfile() const { return m_profile; }
const SourceUpdater &Source::getUpdater() const { return m_updater; }
const QStringList &Source::getAdditionalTokens() const { return m_additionalTokens; }
const QMap<QString, Auth*> &Source::getAuths() const { return m_auths; }

Api *Source::getApi(const QString &name) const
{
	for (Api *api : this->getApis()) {
		if (api->getName() == name) {
			return api;
		}
	}
	return nullptr;
}
