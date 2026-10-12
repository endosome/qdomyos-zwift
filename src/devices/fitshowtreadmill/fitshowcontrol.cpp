#include "fitshowcontrol.h"
namespace FitShow {
Response decodeResponse(const QByteArray &frame) {
    Response response;
    const auto *bytes = reinterpret_cast<const uint8_t *>(frame.constData());
    if (!FitShowPacket::validResponse(bytes, frame.size())) return response;
    response.command = bytes[1];
    response.parameter = bytes[2];
    if (response.command == 0x50) {
        if (response.parameter == 2) {
            if (frame.size() < 7) return response;
            response.limits.maxSpeed = bytes[3]; response.limits.minSpeed = bytes[4];
            if (frame.size() >= 8) response.limits.unit = bytes[5];
        } else if (response.parameter == 3) {
            if (frame.size() < 7) return response; // Short responses cannot prove supported incline bounds.
            response.limits.maxIncline = bytes[3]; response.limits.minIncline = bytes[4];
            if (frame.size() >= 8) response.limits.pause = (bytes[5] & 2) != 0;
        } else if (response.parameter == 5) {
            if (frame.size() < 11) return response;
            response.limits.maxSpeed = bytes[3]; response.limits.minSpeed = bytes[4];
            response.limits.maxIncline = bytes[5]; response.limits.minIncline = bytes[6];
        }
    } else if (response.command == 0x51) {
        response.status.state = response.parameter;
        if (response.parameter == 1 || response.parameter == 3 || response.parameter == 4 || response.parameter == 10) {
            if (frame.size() < 16) return response;
            response.status.hasMotorData = true;
            response.status.speedTenths = bytes[3];
            response.status.inclineCode = int8_t(bytes[4]);
        }
    }
    response.valid = true;
    return response;
}
void Control::clearPending() {
    QList<quint64> canceled;
    if (sent_) canceled.append(inFlight_.id);
    for (const auto &entry : pending_) canceled.append(entry.id);
    sent_ = false;
    pending_.clear();
    for (const auto id : canceled) emit commandEvent(id, "canceled");
}
void Control::cancel() {
    armed_ = false;
    ++generation_;
    clearPending();
}
void Control::setTransport(bool available) {
    if (available_ == available) return;
    available_ = available;
    if (!available) {
        diagnostics_ = false;
        everArmed_ = false;
        limits_ = {};
        cancel();
    }
    emit connectionChanged(available);
}
void Control::allowDiagnostics(bool allowed) {
    diagnostics_ = allowed && available_;
    if (!diagnostics_) cancel();
}
void Control::enqueue(Kind kind, QByteArray payload, quint64 id) {
    pending_.append({kind, std::move(payload), id, generation_});
}
void Control::queryLimits(quint64 id) {
    if (!available_ || !diagnostics_ || armed_) { emit commandEvent(id, "rejected"); return; }
    enqueue(Kind::Query, QByteArray::fromHex("5002"), id);
    enqueue(Kind::Query, QByteArray::fromHex("5003"), id);
}
bool Control::arm(quint64 generation) {
    if (!available_ || !diagnostics_ || !limits_.complete() || generation < generation_) return false;
    generation_ = generation;
    armed_ = everArmed_ = true;
    return true;
}
bool Control::motorRequest(quint64 generation, quint64 id, int speed, int incline) {
    if (!available_ || !diagnostics_ || !armed_ || generation != generation_ || !limits_.accepts(speed, incline)) {
        emit commandEvent(id, "rejected");
        return false;
    }
    return true;
}
void Control::start(quint64 generation, quint64 id, int speed, int incline) {
    if (!motorRequest(generation, id, speed, incline)) return;
    enqueue(Kind::Start, QByteArray::fromHex("53010000000000000000"), id);
    QByteArray pair = QByteArray::fromHex("5302"); pair.append(char(speed)); pair.append(char(incline));
    enqueue(Kind::InitialTarget, pair, id);
}
void Control::target(quint64 generation, quint64 id, int speed, int incline) {
    if (!motorRequest(generation, id, speed, incline)) return;
    QList<quint64> canceled;
    for (int i = pending_.size() - 1; i >= 0; --i) {
        if (pending_[i].kind == Kind::Target) canceled.append(pending_.takeAt(i).id);
    }
    QByteArray pair = QByteArray::fromHex("5302"); pair.append(char(speed)); pair.append(char(incline));
    enqueue(Kind::Target, pair, id);
    for (const auto replaced : canceled) emit commandEvent(replaced, "canceled");
}
void Control::stop(quint64 generation, quint64 id) {
    if (!available_ || !diagnostics_ || !everArmed_ || generation < generation_) {
        emit commandEvent(id, "rejected"); return;
    }
    armed_ = false;
    generation_ = generation;
    clearPending();
    enqueue(Kind::Stop, QByteArray::fromHex("5303"), id);
    pump(nowMs_); // Stop overtakes an outstanding target without waiting for its ACK.
}
void Control::pause(quint64 generation, quint64 id) {
    if (!available_ || !diagnostics_ || !armed_ || generation != generation_ || !limits_.pause) {
        emit commandEvent(id, "rejected"); return;
    }
    clearPending();
    enqueue(Kind::Pause, QByteArray::fromHex("5306"), id);
}
void Control::pump(qint64 nowMs) {
    if (nowMs < nowMs_) { cancel(); emit fault("FitShow clock moved backwards"); return; }
    nowMs_ = nowMs;
    if (!available_ || !diagnostics_) return;
    if (sent_) {
        if (nowMs_ >= deadlineMs_) {
            const auto expired = inFlight_;
            cancel();
            emit commandEvent(expired.id, "timed_out");
            emit fault("FitShow response deadline exceeded; command was not retried");
        }
        return;
    }
    if (!pending_.isEmpty()) {
        inFlight_ = pending_.takeFirst();
        if (inFlight_.generation != generation_) { emit commandEvent(inFlight_.id, "canceled"); return; }
        sent_ = true;
        deadlineMs_ = nowMs_ + 2000;
        const auto command = inFlight_;
        emit commandEvent(command.id, "submitted");
        if (sent_ && generation_ == command.generation && inFlight_.id == command.id && inFlight_.payload == command.payload)
            emit payloadReady(command.payload, command.generation, command.id);
    } else if (nowMs_ - lastPollMs_ >= 500) {
        lastPollMs_ = nowMs_;
        emit payloadReady(QByteArray::fromHex("51"), generation_, 0);
    }
}
void Control::receive(const QByteArray &frame) {
    if (!available_) return;
    const auto response = decodeResponse(frame);
    if (!response.valid) return; // Invalid and unrelated traffic never refreshes motor telemetry.
    if (response.limits.minSpeed >= 0) {
        limits_.minSpeed = response.limits.minSpeed; limits_.maxSpeed = response.limits.maxSpeed;
        limits_.unit = response.limits.unit;
    }
    if (response.limits.minIncline >= 0) {
        limits_.minIncline = response.limits.minIncline; limits_.maxIncline = response.limits.maxIncline;
        limits_.pause = response.limits.pause;
    }
    const bool limitsResponse = response.command == 0x50 && (response.parameter == 2 || response.parameter == 3 || response.parameter == 5);
    if (sent_ && response.command == uint8_t(inFlight_.payload[0]) && response.parameter == uint8_t(inFlight_.payload[1])) {
        const auto id = inFlight_.id;
        sent_ = false;
        emit commandEvent(id, "acknowledged"); // Matching command/subtype is a weak protocol ACK only.
    }
    if (limitsResponse) emit limitsChanged(limits_);
    if (response.command == 0x51) {
        if (response.status.state == 5 || response.status.state == 6 || response.status.state == 7) {
            cancel();
            emit fault("FitShow error, safety or study state");
        }
        emit statusChanged(response.status);
    }
}
}
