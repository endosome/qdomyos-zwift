// treadmill-fit: build, inspect and verify FIT workout files for the W1 (see fitworkout.h).
//   treadmill-fit build <spec.json> <out.fit> [--overwrite]   spec -> FIT, checked by a decode round trip
//   treadmill-fit dump <file.fit>                              FIT -> JSON with totals
//   treadmill-fit check <spec.json> <file.fit>                 exit 0 if the file is exactly what the spec builds
#include "fitworkout.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTextStream>

using namespace Treadmill::Fit;

static int fail(const QString &message, int code = 2) { QTextStream(stderr) << "treadmill-fit: " << message << '\n'; return code; }

static bool readSpec(const QString &path, Training &training, QString &id, QString &error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { error = path + ": " + file.errorString(); return false; }
    if (file.size() > 1024 * 1024) { error = path + ": larger than 1 MiB"; return false; }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) { error = path + ": invalid JSON: " + parse.errorString(); return false; }
    id = document.object().value("id").toString();
    if (id.isEmpty() || QFileInfo(path).completeBaseName() != id) { error = path + ": 'id' must equal the file name without .json"; return false; }
    if (!fromSpec(document.object(), training, error)) { error = path + ": " + error; return false; }
    return true;
}

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments().mid(1);
    if (args.isEmpty()) return fail("usage: build <spec.json> <out.fit> [--overwrite] | dump <file.fit> | check <spec.json> <file.fit>");
    const QString command = args[0];
    QString error, id;
    if (command == "dump" && args.size() == 2) {
        QFile file(args[1]);
        if (!file.open(QIODevice::ReadOnly)) return fail(args[1] + ": " + file.errorString());
        Training training;
        if (!decode(file.readAll(), training, error)) return fail(args[1] + ": " + error, 1);
        QJsonObject json = toJson(training);
        json.insert("id", QFileInfo(args[1]).completeBaseName());
        json.insert("total_seconds", totalSeconds(training));
        json.insert("flat_steps", flatten(training).size());
        json.insert("vertical_meters_estimate", std::round(verticalMetersEstimate(training)));
        QTextStream(stdout) << QJsonDocument(json).toJson(QJsonDocument::Indented);
        return 0;
    }
    if ((command == "build" && (args.size() == 3 || (args.size() == 4 && args[3] == "--overwrite"))) || (command == "check" && args.size() == 3)) {
        Training training;
        if (!readSpec(args[1], training, id, error)) return fail(error);
        const QByteArray bytes = encode(training, error);
        if (bytes.isEmpty()) return fail(error, 1);
        Training back;
        if (!decode(bytes, back, error) || QJsonDocument(toJson(back)) != QJsonDocument(toJson(training)))
            return fail("round trip through the FIT decoder changed the training" + (error.isEmpty() ? QString() : ": " + error), 1);
        if (QFileInfo(args[2]).completeBaseName() != id) return fail(args[2] + ": file name must be '" + id + ".fit'");
        if (command == "check") {
            QFile file(args[2]);
            if (!file.open(QIODevice::ReadOnly)) return fail(args[2] + ": " + file.errorString(), 1);
            if (file.readAll() != bytes) return fail(args[2] + " is not what " + args[1] + " builds; rebuild it", 1);
            return 0;
        }
        if (QFileInfo::exists(args[2]) && args.size() != 4) return fail(args[2] + " exists; pass --overwrite to replace it");
        QFile out(args[2]);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(bytes) != bytes.size()) return fail(args[2] + ": " + out.errorString(), 1);
        QTextStream(stdout) << args[2] << ": " << totalSeconds(training) / 60 << " min, " << flatten(training).size() << " steps, ~"
                            << qRound(verticalMetersEstimate(training)) << " m vertical (estimate)\n";
        return 0;
    }
    return fail("usage: build <spec.json> <out.fit> [--overwrite] | dump <file.fit> | check <spec.json> <file.fit>");
}
