// Read-only scan/info and explicitly approved, bounded nonmotor FitShow probe.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.Advertisement;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Foundation;
using Windows.Storage.Streams;

namespace TreadmillDiagnostics {
    public class Device {
        public string name = "";
        public string address;
        public string address_type;
        public int rssi_dbm;
        public List<string> advertised_service_uuids = new List<string>();
    }
    public class Characteristic {
        public string uuid;
        public ushort handle;
        public string properties;
        public uint properties_bits;
        public string descriptor_status;
        public List<Descriptor> descriptors = new List<Descriptor>();
        public string device_information;
        public string value_hex;
        public string read_status;
    }
    public class Descriptor { public string uuid; public ushort handle; }
    public class Frame {
        public string direction;
        public long monotonic_ms;
        public string utc;
        public string frame_hex;
        public string write_status;
        public string characteristic_uuid;
    }
    public class Service {
        public string uuid;
        public string characteristic_status;
        public List<Characteristic> characteristics = new List<Characteristic>();
    }
    public class Report {
        public string command;
        public string backend = "windows-winrt";
        public string windows_version = Environment.OSVersion.Version.ToString();
        public string status = "complete";
        public string error;
        public string last_operation;
        public string notification_cleanup_status;
        public List<Device> devices = new List<Device>();
        public string address;
        public string address_type;
        public string name;
        public List<string> service_uuids = new List<string>();
        public List<Service> services = new List<Service>();
        public string motor_capabilities = "unknown: no FitShow queries were sent";
        public bool motor_execution_enabled = false;
        public bool status_heart_rate_zero = false;
        public bool status_data_sport = false;
        public bool measure_sleep_timer = false;
        public int idle_initial_state_code = -1;
        public int idle_normal_reports = 0;
        public bool idle_sleep_observed = false;
        public long idle_sleep_after_normal_ms = -1;
        public long idle_first_normal_ms = -1;
        public long idle_initial_sleep_ms = -1;
        public List<Frame> frames = new List<Frame>();
        public string ftms_observation_status = "not requested";
        public List<string> ftms_cleanup_status = new List<string>();
    }
    // Independent Windows-side boundary for the first raw-code motor trial.
    // This is not a calibrated workout profile or a general raw-write API.
    public class MotorTestGate {
        int minimum = -1, maximum = -1, unit = -1, inclineMinimum = -1, inclineMaximum = -1;
        int normalReports = 0;
        long statusAt = -1, startedAt = -1;
        bool stopRequired = false, pauseAttempted = false, readyObserved = false;
        bool pauseSupported = false;
        readonly string[] targets;
        readonly int speedCeiling = 10, inclineCeiling = 0, motorDeadline = 12000;
        readonly bool pauseTrial = false, readyTrial = false, sleepReadyAllowed = false;
        readonly string startFrame = "02530100000000000000005203";
        int targetIndex = 0;
        public MotorTestGate() : this("start-stop") {}
        public MotorTestGate(string trial) : this(trial, "", 0) {}
        public MotorTestGate(string trial, string targetSequence, int timeoutSeconds) {
            targets = new string[] { "0253020a005b03" };
            var parts = trial.Split('-');
            int code;
            if (trial == "workout") {
                if (targetSequence.Length == 0 || targetSequence.Length > 4096 || timeoutSeconds < 1 || timeoutSeconds > 1800)
                    throw new ArgumentException("Workout target sequence/deadline is outside the reviewed limit");
                var pairs = targetSequence.Split(',');
                if (pairs.Length > 256) throw new ArgumentException("Workout has too many target changes");
                var frames = new List<string>();
                int previousSpeed = -1, previousIncline = -1;
                foreach (var pair in pairs) {
                    var fields = pair.Split(':'); int speed, incline;
                    if (fields.Length != 2 || !int.TryParse(fields[0], out speed) || !int.TryParse(fields[1], out incline) ||
                        speed < 10 || speed > 60 || incline < 0 || incline > 8 ||
                        (speed == previousSpeed && incline == previousIncline))
                        throw new ArgumentException("Workout targets must be distinct, validated W1 speed/incline codes");
                    frames.Add(TargetFrame(speed, incline));
                    previousSpeed = speed; previousIncline = incline;
                    speedCeiling = Math.Max(speedCeiling, speed);
                    inclineCeiling = Math.Max(inclineCeiling, incline);
                }
                targets = frames.ToArray();
                motorDeadline = timeoutSeconds * 1000;
            }
            else if (parts.Length == 2 && int.TryParse(parts[1], out code) && parts[0] == "speed" &&
                (code == 12 || code == 15 || code == 20 || code == 30 || code == 40 || code == 50 || code == 60)) {
                speedCeiling = code; motorDeadline = 60000;
                targets = new string[] { TargetFrame(10, 0), TargetFrame(code, 0), TargetFrame(10, 0) };
            } else if (parts.Length == 2 && int.TryParse(parts[1], out code) && parts[0] == "incline" && code >= 1 && code <= 8) {
                inclineCeiling = code; motorDeadline = 80000;
                targets = new string[] { TargetFrame(10, 0), TargetFrame(10, code), TargetFrame(10, 0) };
            } else if (trial == "pause") { pauseTrial = true; motorDeadline = 18000; }
            else if (trial == "incline-timing-4" || trial == "incline-timing-8" || trial == "incline-measure-4" || trial == "incline-measure-8") {
                inclineCeiling = (trial == "incline-timing-8" || trial == "incline-measure-8") ? 8 : 4;
                motorDeadline = (trial == "incline-measure-4" || trial == "incline-measure-8") ? 210000 : 150000;
                targets = new string[] { TargetFrame(10, 0), TargetFrame(10, inclineCeiling), TargetFrame(10, 0) };
            }
            else if (trial == "ready-idle" || trial == "ready-wake") {
                sleepReadyAllowed = trial == "ready-wake";
                readyTrial = true; motorDeadline = 30000;
                startFrame = "0253010000000080000000d203";
                targets = new string[0];
            }
            else if (trial != "start-stop") throw new ArgumentException("Unknown bounded trial");
        }
        static string TargetFrame(int speed, int incline) {
            var bytes = new byte[] { 2, 0x53, 2, (byte)speed, (byte)incline, (byte)(0x53 ^ 2 ^ speed ^ incline), 3 };
            return BitConverter.ToString(bytes).Replace("-", "").ToLowerInvariant();
        }
        public bool Started { get { return startedAt >= 0; } }
        public bool StopAttempted { get; private set; }
        public static byte[] Parse(string hex) {
            if (hex.Length < 8 || hex.Length > 510 || hex.Length % 2 != 0) throw new ArgumentException("Invalid frame length");
            var bytes = new byte[hex.Length / 2];
            for (int i = 0; i < hex.Length; ++i)
                if ("0123456789abcdef".IndexOf(char.ToLowerInvariant(hex[i])) < 0) throw new ArgumentException("Invalid frame hex");
            for (int i = 0; i < bytes.Length; ++i) bytes[i] = Convert.ToByte(hex.Substring(i * 2, 2), 16);
            byte checksum = 0;
            for (int i = 1; i < bytes.Length - 2; ++i) checksum ^= bytes[i];
            if (bytes[0] != 2 || bytes[bytes.Length - 1] != 3 || bytes[bytes.Length - 2] != checksum)
                throw new ArgumentException("Invalid frame envelope/checksum");
            return bytes;
        }
        public void Observe(byte[] bytes, long now) {
            try { Parse(BitConverter.ToString(bytes).Replace("-", "")); } catch { return; }
            if (bytes.Length < 5) return;
            if (bytes[1] == 0x50 && bytes[2] == 2 && bytes.Length >= 8) {
                minimum = bytes[4]; maximum = bytes[3]; unit = bytes[5];
            } else if (bytes[1] == 0x50 && bytes[2] == 3 && bytes.Length >= 7) {
                inclineMinimum = bytes[4]; inclineMaximum = bytes[3];
                pauseSupported = bytes.Length >= 8 && (bytes[5] & 2) != 0;
            } else if (bytes[1] == 0x51) {
                // Only checksum-valid statuses with the lengths used by QZ refresh the clock.
                int state = bytes[2];
                bool motorData = state == 1 || state == 3 || state == 4 || state == 10;
                if (motorData && bytes.Length < 16) return;
                if (state != 0 && state != 2 && !motorData && state != 5 && state != 6 && state != 7 && !(readyTrial && state == 9)) return;
                statusAt = now;
                normalReports = (state == 0 || (sleepReadyAllowed && !Started && state == 6 && bytes.Length >= 6 && bytes[3] == 2))
                    ? normalReports + 1 : 0;
                bool sleepWakeGrace = sleepReadyAllowed && Started && !readyObserved &&
                    state == 6 && bytes.Length >= 6 && bytes[3] == 2 && now - startedAt < 2000;
                if (Started && (state == 5 || (state == 6 && !sleepWakeGrace) || state == 7 || (state == 10 && !pauseAttempted) || state == 1)) stopRequired = true;
                if (Started && readyTrial && state != 0 && state != 9 && !sleepWakeGrace) stopRequired = true;
                if (Started && readyTrial && state == 0 && readyObserved && !StopAttempted) stopRequired = true;
                if (Started && readyTrial && state == 9) readyObserved = true;
                if (Started && motorData && (bytes[3] > speedCeiling || bytes[4] > inclineCeiling || bytes[4] > 127)) stopRequired = true;
            }
            if (Started && !BoundsMatch()) stopRequired = true;
        }
        bool BoundsMatch() { return minimum == 10 && maximum == 60 && unit == 0 && inclineMinimum == 0 && inclineMaximum == 8; }
        public bool NeedsStop(long now) {
            return Started && !StopAttempted && (stopRequired || now - startedAt >= motorDeadline || now - statusAt > 1500 ||
                (sleepReadyAllowed && !readyObserved && now - startedAt >= 2000));
        }
        public byte[] Approve(string hex, long now) {
            var bytes = Parse(hex);
            string value = hex.ToLowerInvariant();
            if (value == "0250025203" || value == "0250035303" || value == "02515103") return bytes;
            if (value == "0253035003") {
                if (!Started || StopAttempted) throw new ArgumentException("Stop is available once after a start attempt");
                StopAttempted = true;
                return bytes;
            }
            if (value == startFrame) {
                if (Started || !BoundsMatch() || normalReports < 3 || statusAt < 0 || now - statusAt > 1500)
                    throw new ArgumentException("Trial preflight requires reviewed walking bounds and three fresh permitted idle reports");
                startedAt = now; // Count attempts, including writes whose completion is uncertain.
                return bytes;
            }
            if (targetIndex < targets.Length && value == targets[targetIndex]) {
                if (!Started || StopAttempted || NeedsStop(now))
                    throw new ArgumentException("Target is unavailable after stop, stale status or trial deadline");
                ++targetIndex;
                return bytes;
            }
            if (value == "0253065503" && pauseTrial && pauseSupported && targetIndex == 1 && !pauseAttempted && Started && !StopAttempted && !NeedsStop(now)) {
                pauseAttempted = true;
                return bytes;
            }
            throw new ArgumentException("Frame is outside the bounded motor trial allowlist");
        }
    }
    public static class Reader {
        static T Await<T>(IAsyncOperation<T> operation, Stopwatch clock, int milliseconds,
                          CancellationToken cancellation = default(CancellationToken)) {
            if (cancellation.IsCancellationRequested) {
                operation.Cancel();
                throw new OperationCanceledException("Diagnostic cancellation requested");
            }
            if (clock.ElapsedMilliseconds >= milliseconds) {
                operation.Cancel();
                throw new TimeoutException("Inspection deadline exceeded");
            }
            while (operation.Status == AsyncStatus.Started) {
                if (cancellation.IsCancellationRequested) {
                    operation.Cancel();
                    throw new OperationCanceledException("Diagnostic cancellation requested");
                }
                if (clock.ElapsedMilliseconds >= milliseconds) {
                    operation.Cancel();
                    throw new TimeoutException("Inspection deadline exceeded");
                }
                Thread.Sleep(20);
            }
            return operation.GetResults();
        }
        static string Address(ulong value) {
            string hex = value.ToString("X12");
            var parts = new string[6];
            for (int i = 0; i < 6; ++i) parts[i] = hex.Substring(i * 2, 2);
            return string.Join(":", parts);
        }
        static ulong ParseAddress(string address) {
            var parts = address.Split(':');
            if (parts.Length != 6) throw new ArgumentException("Device must be an explicit AA:BB:CC:DD:EE:FF address");
            foreach (string part in parts) {
                if (part.Length != 2) throw new ArgumentException("Invalid device address");
                foreach (char digit in part.ToUpperInvariant())
                    if ("0123456789ABCDEF".IndexOf(digit) < 0) throw new ArgumentException("Invalid device address");
            }
            return Convert.ToUInt64(string.Join("", parts), 16);
        }
        public static Report Scan(int seconds) {
            var report = new Report { command = "scan" };
            var clock = Stopwatch.StartNew();
            var adapter = Await(BluetoothAdapter.GetDefaultAsync(), clock, 5000);
            if (adapter == null || !adapter.IsLowEnergySupported) throw new Exception("No Windows BLE adapter available");
            var devices = new Dictionary<ulong, Device>();
            var guard = new object();
            var watcher = new BluetoothLEAdvertisementWatcher();
            watcher.ScanningMode = BluetoothLEScanningMode.Active;
            var handler = new TypedEventHandler<BluetoothLEAdvertisementWatcher, BluetoothLEAdvertisementReceivedEventArgs>((sender, args) => {
                lock (guard) {
                    Device device;
                    if (!devices.TryGetValue(args.BluetoothAddress, out device)) {
                        device = new Device { address = Address(args.BluetoothAddress) };
                        devices.Add(args.BluetoothAddress, device);
                    }
                    device.address_type = args.BluetoothAddressType.ToString().ToLowerInvariant();
                    device.rssi_dbm = args.RawSignalStrengthInDBm;
                    if (!string.IsNullOrEmpty(args.Advertisement.LocalName)) device.name = args.Advertisement.LocalName;
                    foreach (Guid uuid in args.Advertisement.ServiceUuids) {
                        string text = uuid.ToString();
                        if (!device.advertised_service_uuids.Contains(text)) device.advertised_service_uuids.Add(text);
                    }
                }
            });
            // Framework's CodeDOM compiler cannot project WinRT event accessors.
            object token = watcher.GetType().GetMethod("add_Received").Invoke(watcher, new object[] { handler });
            try {
                report.last_operation = "scan_advertisements";
                watcher.Start();
                Thread.Sleep(seconds * 1000);
                if (watcher.Status == BluetoothLEAdvertisementWatcherStatus.Aborted)
                    throw new Exception("Windows BLE scan aborted (check Bluetooth is on and adapter is available)");
            } catch (Exception error) { report.status = "error"; report.error = error.Message; }
            finally {
                watcher.Stop();
                watcher.GetType().GetMethod("remove_Received").Invoke(watcher, new object[] { token });
            }
            lock (guard) {
                report.devices.AddRange(devices.Values);
                report.devices.Sort((left, right) => string.CompareOrdinal(left.address, right.address));
                foreach (Device device in report.devices) device.advertised_service_uuids.Sort(StringComparer.Ordinal);
            }
            return report;
        }
        public static Report Info(string address, string addressType, int seconds) {
            ulong rawAddress = ParseAddress(address);
            var report = new Report { command = "info", address = Address(rawAddress), address_type = addressType };
            var clock = Stopwatch.StartNew();
            var opened = new List<GattDeviceService>();
            BluetoothLEDevice device = null;
            try {
                report.last_operation = "open_device";
                var operation = addressType == "unknown" ? BluetoothLEDevice.FromBluetoothAddressAsync(rawAddress)
                    : BluetoothLEDevice.FromBluetoothAddressAsync(rawAddress,
                        addressType == "random" ? BluetoothAddressType.Random : BluetoothAddressType.Public);
                device = Await(operation, clock, seconds * 1000);
                if (device == null) throw new Exception("Selected BLE device is unavailable; scan first and confirm its address");
                report.name = device.Name;
                report.address_type = device.BluetoothAddressType.ToString().ToLowerInvariant();
                report.last_operation = "discover_services";
                var found = Await(device.GetGattServicesAsync(BluetoothCacheMode.Uncached), clock, seconds * 1000);
                if (found.Status != GattCommunicationStatus.Success) throw new Exception("Service discovery: " + found.Status);
                opened.AddRange(found.Services);
                foreach (var service in opened) report.service_uuids.Add(service.Uuid.ToString());
                report.service_uuids.Sort(StringComparer.Ordinal);
                foreach (var service in opened) {
                    var entry = new Service { uuid = service.Uuid.ToString() };
                    report.services.Add(entry);
                    report.last_operation = "discover_characteristics " + entry.uuid;
                    var characteristics = Await(service.GetCharacteristicsAsync(BluetoothCacheMode.Uncached), clock, seconds * 1000);
                    entry.characteristic_status = characteristics.Status.ToString();
                    if (characteristics.Status != GattCommunicationStatus.Success) throw new Exception("Characteristic discovery: " + characteristics.Status);
                    foreach (var characteristic in characteristics.Characteristics) {
                        var item = new Characteristic { uuid = characteristic.Uuid.ToString(), handle = characteristic.AttributeHandle,
                            properties = characteristic.CharacteristicProperties.ToString(), properties_bits = (uint)characteristic.CharacteristicProperties };
                        entry.characteristics.Add(item);
                        report.last_operation = "discover_descriptors " + item.uuid;
                        var descriptors = Await(characteristic.GetDescriptorsAsync(BluetoothCacheMode.Uncached), clock, seconds * 1000);
                        item.descriptor_status = descriptors.Status.ToString();
                        if (descriptors.Status != GattCommunicationStatus.Success) throw new Exception("Descriptor discovery: " + descriptors.Status);
                        foreach (var descriptor in descriptors.Descriptors)
                            item.descriptors.Add(new Descriptor { uuid = descriptor.Uuid.ToString(), handle = descriptor.AttributeHandle });
                        string shortUuid = item.uuid.Substring(0, 8);
                        bool identityValue = entry.uuid == "0000180a-0000-1000-8000-00805f9b34fb" &&
                            (shortUuid == "00002a24" || shortUuid == "00002a26" || shortUuid == "00002a27" || shortUuid == "00002a28" || shortUuid == "00002a29");
                        bool ftmsValue = entry.uuid == "00001826-0000-1000-8000-00805f9b34fb" &&
                            (shortUuid == "00002acc" || shortUuid == "00002ad4" || shortUuid == "00002ad5" ||
                             shortUuid == "00002ad6" || shortUuid == "00002ad7" || shortUuid == "00002ad8");
                        if ((identityValue || ftmsValue) &&
                            (characteristic.CharacteristicProperties & GattCharacteristicProperties.Read) != 0) {
                            report.last_operation = "read_standard_value " + item.uuid;
                            var value = Await(characteristic.ReadValueAsync(BluetoothCacheMode.Uncached), clock, seconds * 1000);
                            item.read_status = value.Status.ToString();
                            if (value.Status == GattCommunicationStatus.Success) {
                                using (var reader = DataReader.FromBuffer(value.Value)) {
                                    var bytes = new byte[reader.UnconsumedBufferLength];
                                    reader.ReadBytes(bytes);
                                    item.value_hex = BitConverter.ToString(bytes).Replace("-", "").ToLowerInvariant();
                                    if (identityValue) item.device_information = System.Text.Encoding.UTF8.GetString(bytes);
                                }
                            }
                        }
                    }
                }
            } catch (Exception error) { report.status = "error"; report.error = error.Message; }
            finally {
                foreach (var service in opened) service.Dispose();
                if (device != null) device.Dispose();
            }
            return report;
        }
        // Only these three payloads can reach the write path. There is no arm,
        // initialization replay, motor command, pause, stop or generic raw write.
        public static byte[] FrameForQuery(int query) { return FrameForQuery(query, false); }
        public static byte[] FrameForQuery(int query, bool statusHeartRateZero) {
            byte[] payload;
            if (query == 0x5002) payload = new byte[] { 0x50, 0x02 };
            else if (query == 0x5003) payload = new byte[] { 0x50, 0x03 };
            else if (query == 0x51) payload = statusHeartRateZero ? new byte[] { 0x51, 0x00 } : new byte[] { 0x51 };
            else if (query == 0x5200) payload = new byte[] { 0x52, 0x00 };
            else throw new ArgumentException("Only documented nonmotor queries 5002, 5003, 51 and 5200 are allowed");
            var frame = new byte[payload.Length + 3];
            frame[0] = 0x02;
            byte checksum = 0;
            for (int i = 0; i < payload.Length; ++i) { frame[i + 1] = payload[i]; checksum ^= payload[i]; }
            frame[frame.Length - 2] = checksum;
            frame[frame.Length - 1] = 0x03;
            return frame;
        }
        // Invalid/unrelated frames do not count as evidence of an idle state.
        public static bool RecordIdleStatus(Report report, string hex, long now) {
            byte[] bytes;
            try { bytes = MotorTestGate.Parse(hex); } catch { return true; }
            if (bytes.Length < 5 || bytes[1] != 0x51) return true;
            int state = bytes[2];
            if (report.idle_initial_state_code < 0) report.idle_initial_state_code = state;
            if (state == 0) {
                report.idle_normal_reports++;
                if (report.idle_first_normal_ms < 0) report.idle_first_normal_ms = now;
                return true;
            }
            if (state == 6 && bytes.Length >= 6 && bytes[3] == 2) {
                if (report.idle_normal_reports == 0 && report.idle_initial_sleep_ms < 0) report.idle_initial_sleep_ms = now;
                report.idle_sleep_observed = true;
                if (report.idle_first_normal_ms >= 0) report.idle_sleep_after_normal_ms = now - report.idle_first_normal_ms;
                return true;
            }
            return false;
        }
        public static Report Probe(string address, string addressType, string expectedName, int seconds, bool approved) {
            return Probe(address, addressType, expectedName, seconds, approved, false);
        }
        public static Report Probe(string address, string addressType, string expectedName, int seconds, bool approved, bool bridge) {
            return Probe(address, addressType, expectedName, seconds, approved, bridge, false);
        }
        public static Report Probe(string address, string addressType, string expectedName, int seconds, bool approved, bool bridge, bool observeFtms) {
            return Probe(address, addressType, expectedName, seconds, approved, bridge, observeFtms, false);
        }
        public static Report Probe(string address, string addressType, string expectedName, int seconds, bool approved, bool bridge, bool observeFtms, bool statusHeartRateZero) {
            return Probe(address, addressType, expectedName, seconds, approved, bridge, observeFtms, statusHeartRateZero, false, false);
        }
        public static Report Probe(string address, string addressType, string expectedName, int seconds, bool approved, bool bridge, bool observeFtms, bool statusHeartRateZero, bool statusDataSport) {
            return Probe(address, addressType, expectedName, seconds, approved, bridge, observeFtms, statusHeartRateZero, statusDataSport, false);
        }
        public static Report Probe(string address, string addressType, string expectedName, int seconds, bool approved, bool bridge, bool observeFtms, bool statusHeartRateZero, bool statusDataSport, bool measureSleepTimer) {
            if (!approved || string.IsNullOrEmpty(expectedName)) throw new ArgumentException("Nonmotor probe approval/name required");
            if ((statusHeartRateZero && statusDataSport) || (measureSleepTimer && (statusHeartRateZero || statusDataSport))) throw new ArgumentException("Select only one experimental status mode");
            return Session(address, addressType, expectedName, seconds, bridge, false, "start-stop", observeFtms, statusHeartRateZero, statusDataSport, measureSleepTimer);
        }
        public static Report ControlTest(string address, string addressType, string expectedName, bool approved) {
            return ControlTest(address, addressType, expectedName, approved, "start-stop");
        }
        public static Report ControlTest(string address, string addressType, string expectedName, bool approved, string trial) {
            return ControlTest(address, addressType, expectedName, approved, trial, false);
        }
        public static Report ControlTest(string address, string addressType, string expectedName, bool approved, string trial, bool observeFtms) {
            return ControlTest(address, addressType, expectedName, approved, trial, observeFtms, "", 120);
        }
        public static Report ControlTest(string address, string addressType, string expectedName, bool approved, string trial, bool observeFtms, string targetSequence, int timeoutSeconds) {
            if (trial == "ready-idle" || trial == "ready-wake") throw new ArgumentException("READY trials retired: W1 entered countdown and stop was not verified");
            if (!approved || string.IsNullOrEmpty(expectedName)) throw new ArgumentException("Bounded MOTOR trial approval/name required");
            if (trial == "workout") return Session(address, addressType, expectedName, timeoutSeconds, true, true, trial, observeFtms, false, false, false, targetSequence);
            return Session(address, addressType, expectedName, (trial == "incline-timing-4" || trial == "incline-timing-8" || trial == "incline-measure-4" || trial == "incline-measure-8") ? 240 : 120, true, true, trial, observeFtms);
        }
        static Report Session(string address, string addressType, string expectedName, int seconds, bool bridge, bool motor,
                              string trial = "start-stop", bool observeFtms = false, bool statusHeartRateZero = false, bool statusDataSport = false, bool measureSleepTimer = false, string targetSequence = "") {
            if (seconds < 1 || seconds > (motor ? (trial == "workout" ? 1800 : 240) : (measureSleepTimer ? 900 : 600))) throw new ArgumentException("Session duration exceeds its bounded scope");
            if (motor && (statusHeartRateZero || statusDataSport || measureSleepTimer)) throw new ArgumentException("Zero heart-rate status is nonmotor only");
            ulong rawAddress = ParseAddress(address);
            var report = new Report { status_heart_rate_zero = statusHeartRateZero, status_data_sport = statusDataSport, measure_sleep_timer = measureSleepTimer, command = motor ? "control-test" : "probe", address = Address(rawAddress), address_type = addressType,
                motor_execution_enabled = motor,
                motor_capabilities = "unknown: raw diagnostic replies only; no physical behavior verified" };
            var clock = Stopwatch.StartNew();
            BluetoothLEDevice device = null;
            var opened = new List<GattDeviceService>();
            GattCharacteristic notify = null;
            GattCharacteristic write = null;
            var ftmsNotifies = new List<GattCharacteristic>();
            var ftmsTokens = new List<object>();
            object token = null;
            bool subscribed = false;
            bool overflow = false, collecting = true;
            var cancellation = new CancellationTokenSource();
            int capturedCharacters = 0;
            bool idleUnexpectedState = false;
            bool sleepReadyAnnounced = false;
            var guard = new object();
            var commands = new Queue<string>();
            var output = new Queue<string>();
            var gate = new MotorTestGate(trial, targetSequence, seconds);
            // A stalled parent must not block the WinRT callback or motor watchdog.
            Action<string> stream = (line) => {
                if (!motor) return;
                lock (guard) {
                    if (output.Count >= 256) { overflow = true; cancellation.Cancel(); return; }
                    output.Enqueue(line);
                }
            };
            Thread outputThread = null;
            if (motor) {
                outputThread = new Thread(() => {
                    try {
                        while (true) {
                            string line = null;
                            lock (guard) {
                                if (output.Count > 0) line = output.Dequeue();
                                else if (!collecting) return;
                            }
                            if (line == null) { Thread.Sleep(20); continue; }
                            Console.WriteLine(line); Console.Out.Flush();
                        }
                    } catch { lock (guard) { if (collecting) cancellation.Cancel(); } }
                });
                outputThread.IsBackground = true;
                outputThread.Start();
            }
            ConsoleCancelEventHandler cancel = (sender, args) => { args.Cancel = true; cancellation.Cancel(); };
            Console.CancelKeyPress += cancel;
            if (bridge) {
                // A parent CLI sends QUIT, or closes the pipe on exit. Keep WinRT
                // teardown on this MTA thread; the reader only requests cancellation.
                var input = new Thread(() => {
                    try {
                        while (true) {
                            string line = Console.In.ReadLine();
                            lock (guard) {
                                if (!collecting) return;
                                if (line == null || line == "QUIT") { cancellation.Cancel(); return; }
                                if (!motor || line.Length > 80 || commands.Count >= 16) { cancellation.Cancel(); return; }
                                commands.Enqueue(line);
                            }
                        }
                    } catch { lock (guard) { if (collecting) cancellation.Cancel(); } }
                });
                input.IsBackground = true;
                input.Start();
            }
            try {
                report.last_operation = "open_device";
                var operation = addressType == "unknown" ? BluetoothLEDevice.FromBluetoothAddressAsync(rawAddress)
                    : BluetoothLEDevice.FromBluetoothAddressAsync(rawAddress,
                        addressType == "random" ? BluetoothAddressType.Random : BluetoothAddressType.Public);
                device = Await(operation, clock, seconds * 1000, cancellation.Token);
                if (device == null || device.Name != expectedName) throw new Exception("Selected device name unavailable or differs from the operator-confirmed name");
                report.name = device.Name;
                if (motor) {
                    report.last_operation = "verify_fitshow_module_identity";
                    var identity = Await(device.GetGattServicesForUuidAsync(new Guid("0000180a-0000-1000-8000-00805f9b34fb"), BluetoothCacheMode.Uncached), clock, seconds * 1000, cancellation.Token);
                    opened.AddRange(identity.Services);
                    if (identity.Status != GattCommunicationStatus.Success || identity.Services.Count != 1)
                        throw new Exception("Module identity service unavailable; motor control remains blocked");
                    var fields = Await(identity.Services[0].GetCharacteristicsAsync(BluetoothCacheMode.Uncached), clock, seconds * 1000, cancellation.Token);
                    if (fields.Status != GattCommunicationStatus.Success) throw new Exception("Module identity characteristics unavailable");
                    var values = new Dictionary<string, string>();
                    foreach (var field in fields.Characteristics) {
                        string key = field.Uuid.ToString().Substring(0, 8);
                        if (key != "00002a24" && key != "00002a28" && key != "00002a29") continue;
                        var value = Await(field.ReadValueAsync(BluetoothCacheMode.Uncached), clock, seconds * 1000, cancellation.Token);
                        if (value.Status != GattCommunicationStatus.Success) throw new Exception("Module identity value unreadable");
                        using (var reader = DataReader.FromBuffer(value.Value)) {
                            var bytes = new byte[reader.UnconsumedBufferLength]; reader.ReadBytes(bytes);
                            values[key] = System.Text.Encoding.UTF8.GetString(bytes).TrimEnd('\0');
                        }
                    }
                    if (!values.ContainsKey("00002a29") || values["00002a29"] != "FITSHOW" ||
                        !values.ContainsKey("00002a24") || values["00002a24"] != "FS-BT-C1" ||
                        !values.ContainsKey("00002a28") || values["00002a28"] != "1.3.8")
                        throw new Exception("Module identity differs from the reviewed W1 evidence");
                    stream("IDENTITY\tFITSHOW\tFS-BT-C1\t1.3.8");
                }
                report.last_operation = "discover_fitshow_service";
                var services = Await(device.GetGattServicesForUuidAsync(new Guid("0000fff0-0000-1000-8000-00805f9b34fb"), BluetoothCacheMode.Uncached), clock, seconds * 1000, cancellation.Token);
                opened.AddRange(services.Services);
                if (services.Status != GattCommunicationStatus.Success || services.Services.Count != 1) throw new Exception("Expected exactly one accessible FFF0 service");
                var characteristics = Await(services.Services[0].GetCharacteristicsAsync(BluetoothCacheMode.Uncached), clock, seconds * 1000, cancellation.Token);
                if (characteristics.Status != GattCommunicationStatus.Success) throw new Exception("FitShow characteristic discovery failed");
                foreach (var item in characteristics.Characteristics) {
                    if (item.Uuid == new Guid("0000fff1-0000-1000-8000-00805f9b34fb")) notify = item;
                    if (item.Uuid == new Guid("0000fff2-0000-1000-8000-00805f9b34fb")) write = item;
                }
                if (notify == null || write == null || (notify.CharacteristicProperties & GattCharacteristicProperties.Notify) == 0 ||
                    (write.CharacteristicProperties & GattCharacteristicProperties.WriteWithoutResponse) == 0)
                    throw new Exception("Expected FFF1 Notify and FFF2 WriteWithoutResponse properties");
                var handler = new TypedEventHandler<GattCharacteristic, GattValueChangedEventArgs>((sender, args) => {
                    using (var reader = DataReader.FromBuffer(args.CharacteristicValue)) {
                        var bytes = new byte[reader.UnconsumedBufferLength]; reader.ReadBytes(bytes);
                        string hex = BitConverter.ToString(bytes).Replace("-", "").ToLowerInvariant();
                        lock (guard) {
                            if (!collecting) return;
                            if (report.frames.Count >= 4096 || capturedCharacters + hex.Length > 6 * 1024 * 1024) { overflow = true; return; }
                            capturedCharacters += hex.Length;
                            report.frames.Add(new Frame { direction = "rx", frame_hex = hex, monotonic_ms = clock.ElapsedMilliseconds,
                                utc = DateTime.UtcNow.ToString("o"), characteristic_uuid = notify.Uuid.ToString() });
                            if ((statusHeartRateZero || statusDataSport || measureSleepTimer) && !RecordIdleStatus(report, hex, clock.ElapsedMilliseconds)) idleUnexpectedState = true;
                            if (measureSleepTimer && report.idle_normal_reports == 0 && report.idle_sleep_observed) report.idle_sleep_observed = false;
                            if (motor) { gate.Observe(bytes, clock.ElapsedMilliseconds); stream("RX\t" + hex); }
                        }
                    }
                });
                token = notify.GetType().GetMethod("add_ValueChanged").Invoke(notify, new object[] { handler });
                report.last_operation = "enable_notifications";
                // Cancellation/timeout may occur after the device applied the
                // CCCD write. Attempt cleanup whenever enabling was requested.
                subscribed = true;
                var subscription = Await(notify.WriteClientCharacteristicConfigurationDescriptorAsync(GattClientCharacteristicConfigurationDescriptorValue.Notify), clock, seconds * 1000, cancellation.Token);
                if (subscription != GattCommunicationStatus.Success) throw new Exception("Notification subscription failed: " + subscription);
                if (observeFtms) {
                    report.last_operation = "discover_ftms_observation";
                    report.ftms_observation_status = "discovery pending";
                    var ftms = Await(device.GetGattServicesForUuidAsync(new Guid("00001826-0000-1000-8000-00805f9b34fb"), BluetoothCacheMode.Uncached), clock, seconds * 1000, cancellation.Token);
                    opened.AddRange(ftms.Services);
                    if (ftms.Status != GattCommunicationStatus.Success || ftms.Services.Count != 1)
                        throw new Exception("FTMS observation service unavailable: " + ftms.Status);
                    var fields = Await(ftms.Services[0].GetCharacteristicsAsync(BluetoothCacheMode.Uncached), clock, seconds * 1000, cancellation.Token);
                    if (fields.Status != GattCommunicationStatus.Success) throw new Exception("FTMS observation characteristics unavailable");
                    foreach (var item in fields.Characteristics) {
                        string uuid = item.Uuid.ToString();
                        if (uuid != "00002acd-0000-1000-8000-00805f9b34fb" && uuid != "00002ada-0000-1000-8000-00805f9b34fb") continue;
                        if ((item.CharacteristicProperties & GattCharacteristicProperties.Notify) == 0) throw new Exception("FTMS characteristic is not Notify: " + uuid);
                        var ftmsCharacteristic = item;
                        var ftmsHandler = new TypedEventHandler<GattCharacteristic, GattValueChangedEventArgs>((sender, args) => {
                            using (var reader = DataReader.FromBuffer(args.CharacteristicValue)) {
                                var bytes = new byte[reader.UnconsumedBufferLength]; reader.ReadBytes(bytes);
                                string hex = BitConverter.ToString(bytes).Replace("-", "").ToLowerInvariant();
                                lock (guard) {
                                    if (!collecting) return;
                                    if (report.frames.Count >= 4096 || capturedCharacters + hex.Length > 6 * 1024 * 1024) { overflow = true; return; }
                                    capturedCharacters += hex.Length;
                                    report.frames.Add(new Frame { direction = "rx", frame_hex = hex, characteristic_uuid = ftmsCharacteristic.Uuid.ToString(),
                                        monotonic_ms = clock.ElapsedMilliseconds, utc = DateTime.UtcNow.ToString("o") });
                                    // Never feed unknown FTMS data into FitShow motor authorization/freshness.
                                    stream("FTMS\t" + ftmsCharacteristic.Uuid + "\t" + hex);
                                }
                            }
                        });
                        ftmsTokens.Add(item.GetType().GetMethod("add_ValueChanged").Invoke(item, new object[] { ftmsHandler }));
                        ftmsNotifies.Add(item); // Track attempts so cancellation still cleans up CCCDs.
                        var status = Await(item.WriteClientCharacteristicConfigurationDescriptorAsync(GattClientCharacteristicConfigurationDescriptorValue.Notify), clock, seconds * 1000, cancellation.Token);
                        if (status != GattCommunicationStatus.Success) throw new Exception("FTMS notification subscription failed: " + status);
                    }
                    if (ftmsNotifies.Count != 2) throw new Exception("FTMS observation requires 2ACD and 2ADA");
                    report.ftms_observation_status = "complete";
                }
                stream("READY\t" + report.name);
                int information = 0;
                long nextQuery = clock.ElapsedMilliseconds;
                bool sportNext = false;
                while (!cancellation.IsCancellationRequested && clock.ElapsedMilliseconds < seconds * 1000) {
                    bool announceSleepReady = false;
                    lock (guard) {
                        if (overflow) throw new Exception("Diagnostic trace limit reached; closing connection");
                        if ((statusHeartRateZero || statusDataSport || measureSleepTimer) && idleUnexpectedState) throw new Exception("Idle observer requires sleep/NORMAL status; unexpected state observed. Use physical stop if moving.");
                        if (measureSleepTimer && report.idle_initial_state_code >= 0 && report.idle_initial_state_code != 6) throw new Exception("Sleep-timer measurement must begin in documented sleep 06/02");
                        if (measureSleepTimer && report.idle_initial_state_code == 6 && !sleepReadyAnnounced) { sleepReadyAnnounced = true; announceSleepReady = true; }
                        if (measureSleepTimer && report.idle_initial_sleep_ms >= 0 && report.idle_normal_reports == 0 && clock.ElapsedMilliseconds - report.idle_initial_sleep_ms > 90000) throw new Exception("No physical wake within 90 seconds of initial sleep; closing observer");
                        if ((statusHeartRateZero || statusDataSport || measureSleepTimer) && report.idle_sleep_observed) break;
                    }
                    if (announceSleepReady) Console.Error.WriteLine("SLEEP_TIMER_READY");
                    if (motor) {
                        string command = null;
                        lock (guard) {
                            if (gate.NeedsStop(clock.ElapsedMilliseconds)) throw new Exception("Windows motor watchdog requested stop");
                            if (commands.Count > 0) command = commands.Dequeue();
                        }
                        if (command == null) { Thread.Sleep(20); continue; }
                        var parts = command.Split('\t');
                        ulong id;
                        if (parts.Length != 3 || parts[0] != "WRITE" || !ulong.TryParse(parts[1], out id))
                            throw new Exception("Invalid control bridge command");
                        byte[] frame;
                        lock (guard) { frame = gate.Approve(parts[2], clock.ElapsedMilliseconds); }
                        report.last_operation = "bounded_motor_frame " + parts[2];
                        var status = WriteFrame(write, frame, report, guard, clock, 1000);
                        stream("TX\t" + parts[1] + "\t" + parts[2] + "\t" + status);
                        if (status != GattCommunicationStatus.Success) throw new Exception("Control write failed: " + status);
                        continue;
                    }
                    if (clock.ElapsedMilliseconds < nextQuery) { Thread.Sleep(20); continue; }
                    int query = information == 0 ? 0x5002 : (information == 1 ? 0x5003 : (statusDataSport && sportNext ? 0x5200 : 0x51));
                    if (information >= 2 && statusDataSport) sportNext = !sportNext;
                    information = Math.Min(2, information + 1);
                    var bytes = FrameForQuery(query, statusHeartRateZero);
                    report.last_operation = "nonmotor_query " + query.ToString("x");
                    using (var writer = new DataWriter()) {
                        writer.WriteBytes(bytes);
                        var status = Await(write.WriteValueAsync(writer.DetachBuffer(), GattWriteOption.WriteWithoutResponse), clock, seconds * 1000, cancellation.Token);
                        lock (guard) report.frames.Add(new Frame { direction = "tx", frame_hex = BitConverter.ToString(bytes).Replace("-", "").ToLowerInvariant(),
                            write_status = status.ToString(), monotonic_ms = clock.ElapsedMilliseconds, utc = DateTime.UtcNow.ToString("o") });
                        if (status != GattCommunicationStatus.Success) throw new Exception("Diagnostic write failed: " + status);
                    }
                    nextQuery = clock.ElapsedMilliseconds + 500;
                }
            } catch (Exception error) { report.status = "error"; report.error = error.Message; }
            finally {
                if (motor && gate.Started && !gate.StopAttempted && write != null) {
                    try {
                        byte[] stop;
                        lock (guard) { commands.Clear(); stop = gate.Approve("0253035003", clock.ElapsedMilliseconds); }
                        var status = WriteFrame(write, stop, report, guard, clock, 1000);
                        stream("TX\t0\t0253035003\t" + status);
                    } catch (Exception error) { report.status = "error"; report.error = "Emergency stop unconfirmed: " + error.Message; }
                }
                lock (guard) { collecting = false; }
                Console.CancelKeyPress -= cancel;
                for (int i = 0; i < ftmsNotifies.Count; ++i) {
                    var item = ftmsNotifies[i];
                    try {
                        var status = Await(item.WriteClientCharacteristicConfigurationDescriptorAsync(GattClientCharacteristicConfigurationDescriptorValue.None), Stopwatch.StartNew(), 2000);
                        report.ftms_cleanup_status.Add(item.Uuid + ": " + status);
                        if (status != GattCommunicationStatus.Success) { report.status = "error"; report.error = "FTMS notification cleanup unconfirmed"; }
                    } catch (Exception error) { report.ftms_cleanup_status.Add(item.Uuid + ": unconfirmed " + error.Message); report.status = "error"; }
                    try { item.GetType().GetMethod("remove_ValueChanged").Invoke(item, new object[] { ftmsTokens[i] }); } catch { report.status = "error"; }
                }
                if (subscribed) {
                    try { report.notification_cleanup_status = Await(notify.WriteClientCharacteristicConfigurationDescriptorAsync(GattClientCharacteristicConfigurationDescriptorValue.None), Stopwatch.StartNew(), 2000).ToString(); }
                    catch (Exception error) { report.notification_cleanup_status = "unconfirmed: " + error.Message; }
                }
                if (notify != null && token != null) {
                    try { notify.GetType().GetMethod("remove_ValueChanged").Invoke(notify, new object[] { token }); }
                    catch { /* Always dispose services/device even if event teardown fails. */ }
                }
                foreach (var service in opened) {
                    try { service.Dispose(); } catch { /* Continue releasing all resources. */ }
                }
                if (device != null) {
                    try { device.Dispose(); } catch { /* No more requests can be sent. */ }
                }
                cancellation.Dispose();
                if (outputThread != null) outputThread.Join(1000);
            }
            return report;
        }
        static GattCommunicationStatus WriteFrame(GattCharacteristic write, byte[] bytes, Report report,
                                                   object guard, Stopwatch clock, int timeout) {
            using (var writer = new DataWriter()) {
                writer.WriteBytes(bytes);
                // Log the attempt before native I/O; timeout cannot hide a possible write.
                var frame = new Frame { direction = "tx", frame_hex = BitConverter.ToString(bytes).Replace("-", "").ToLowerInvariant(),
                    write_status = "attempted", characteristic_uuid = write.Uuid.ToString(),
                    monotonic_ms = clock.ElapsedMilliseconds, utc = DateTime.UtcNow.ToString("o") };
                lock (guard) report.frames.Add(frame);
                var status = Await(write.WriteValueAsync(writer.DetachBuffer(), GattWriteOption.WriteWithoutResponse), clock,
                                   (int)clock.ElapsedMilliseconds + timeout);
                lock (guard) frame.write_status = status.ToString();
                return status;
            }
        }
    }
}
