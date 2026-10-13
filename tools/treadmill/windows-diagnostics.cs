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
        public List<Frame> frames = new List<Frame>();
    }
    public static class Reader {
        static T Await<T>(IAsyncOperation<T> operation, Stopwatch clock, int milliseconds) {
            if (clock.ElapsedMilliseconds >= milliseconds) {
                operation.Cancel();
                throw new TimeoutException("Inspection deadline exceeded");
            }
            while (operation.Status == AsyncStatus.Started) {
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
        public static byte[] FrameForQuery(int query) {
            byte[] payload;
            if (query == 0x5002) payload = new byte[] { 0x50, 0x02 };
            else if (query == 0x5003) payload = new byte[] { 0x50, 0x03 };
            else if (query == 0x51) payload = new byte[] { 0x51 };
            else throw new ArgumentException("Only nonmotor queries 5002, 5003 and 51 are allowed");
            var frame = new byte[payload.Length + 3];
            frame[0] = 0x02;
            byte checksum = 0;
            for (int i = 0; i < payload.Length; ++i) { frame[i + 1] = payload[i]; checksum ^= payload[i]; }
            frame[frame.Length - 2] = checksum;
            frame[frame.Length - 1] = 0x03;
            return frame;
        }
        public static Report Probe(string address, string addressType, string expectedName, int seconds, bool approved) {
            if (!approved || string.IsNullOrEmpty(expectedName)) throw new ArgumentException("Nonmotor probe approval/name required");
            if (seconds < 1 || seconds > 60) throw new ArgumentException("Probe duration must be 1-60 seconds");
            ulong rawAddress = ParseAddress(address);
            var report = new Report { command = "probe", address = Address(rawAddress), address_type = addressType,
                motor_capabilities = "unknown: raw diagnostic replies only; no physical behavior verified" };
            var clock = Stopwatch.StartNew();
            BluetoothLEDevice device = null;
            var opened = new List<GattDeviceService>();
            GattCharacteristic notify = null;
            object token = null;
            bool subscribed = false;
            bool overflow = false, collecting = true;
            var cancellation = new CancellationTokenSource();
            int capturedCharacters = 0;
            var guard = new object();
            ConsoleCancelEventHandler cancel = (sender, args) => { args.Cancel = true; cancellation.Cancel(); };
            Console.CancelKeyPress += cancel;
            try {
                report.last_operation = "open_device";
                var operation = addressType == "unknown" ? BluetoothLEDevice.FromBluetoothAddressAsync(rawAddress)
                    : BluetoothLEDevice.FromBluetoothAddressAsync(rawAddress,
                        addressType == "random" ? BluetoothAddressType.Random : BluetoothAddressType.Public);
                device = Await(operation, clock, seconds * 1000);
                if (device == null || device.Name != expectedName) throw new Exception("Selected device name unavailable or differs from the operator-confirmed name");
                report.name = device.Name;
                report.last_operation = "discover_fitshow_service";
                var services = Await(device.GetGattServicesForUuidAsync(new Guid("0000fff0-0000-1000-8000-00805f9b34fb"), BluetoothCacheMode.Uncached), clock, seconds * 1000);
                opened.AddRange(services.Services);
                if (services.Status != GattCommunicationStatus.Success || opened.Count != 1) throw new Exception("Expected exactly one accessible FFF0 service");
                var characteristics = Await(opened[0].GetCharacteristicsAsync(BluetoothCacheMode.Uncached), clock, seconds * 1000);
                if (characteristics.Status != GattCommunicationStatus.Success) throw new Exception("FitShow characteristic discovery failed");
                GattCharacteristic write = null;
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
                            report.frames.Add(new Frame { direction = "rx", frame_hex = hex, monotonic_ms = clock.ElapsedMilliseconds, utc = DateTime.UtcNow.ToString("o") });
                        }
                    }
                });
                token = notify.GetType().GetMethod("add_ValueChanged").Invoke(notify, new object[] { handler });
                report.last_operation = "enable_notifications";
                var subscription = Await(notify.WriteClientCharacteristicConfigurationDescriptorAsync(GattClientCharacteristicConfigurationDescriptorValue.Notify), clock, seconds * 1000);
                if (subscription != GattCommunicationStatus.Success) throw new Exception("Notification subscription failed: " + subscription);
                subscribed = true;
                int information = 0;
                long nextQuery = clock.ElapsedMilliseconds;
                while (!cancellation.IsCancellationRequested && clock.ElapsedMilliseconds < seconds * 1000) {
                    lock (guard) { if (overflow) throw new Exception("Diagnostic trace limit reached; closing connection"); }
                    if (clock.ElapsedMilliseconds < nextQuery) { Thread.Sleep(20); continue; }
                    int query = information == 0 ? 0x5002 : (information == 1 ? 0x5003 : 0x51);
                    information = Math.Min(2, information + 1);
                    var bytes = FrameForQuery(query);
                    report.last_operation = "nonmotor_query " + query.ToString("x");
                    using (var writer = new DataWriter()) {
                        writer.WriteBytes(bytes);
                        var status = Await(write.WriteValueAsync(writer.DetachBuffer(), GattWriteOption.WriteWithoutResponse), clock, seconds * 1000);
                        lock (guard) report.frames.Add(new Frame { direction = "tx", frame_hex = BitConverter.ToString(bytes).Replace("-", "").ToLowerInvariant(),
                            write_status = status.ToString(), monotonic_ms = clock.ElapsedMilliseconds, utc = DateTime.UtcNow.ToString("o") });
                        if (status != GattCommunicationStatus.Success) throw new Exception("Diagnostic write failed: " + status);
                    }
                    nextQuery = clock.ElapsedMilliseconds + 500;
                }
            } catch (Exception error) { report.status = "error"; report.error = error.Message; }
            finally {
                lock (guard) { collecting = false; }
                Console.CancelKeyPress -= cancel;
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
            }
            return report;
        }
    }
}
