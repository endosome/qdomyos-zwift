// Read-only Windows bridge for WSL diagnostics. No control protocol is here.
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
        public string read_status;
    }
    public class Descriptor { public string uuid; public ushort handle; }
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
        public List<Device> devices = new List<Device>();
        public string address;
        public string address_type;
        public string name;
        public List<string> service_uuids = new List<string>();
        public List<Service> services = new List<Service>();
        public string motor_capabilities = "unknown: no FitShow queries were sent";
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
                        if (entry.uuid == "0000180a-0000-1000-8000-00805f9b34fb" &&
                            (shortUuid == "00002a24" || shortUuid == "00002a26" || shortUuid == "00002a27" || shortUuid == "00002a29") &&
                            (characteristic.CharacteristicProperties & GattCharacteristicProperties.Read) != 0) {
                            report.last_operation = "read_device_information " + item.uuid;
                            var value = Await(characteristic.ReadValueAsync(BluetoothCacheMode.Uncached), clock, seconds * 1000);
                            item.read_status = value.Status.ToString();
                            if (value.Status == GattCommunicationStatus.Success) {
                                using (var reader = DataReader.FromBuffer(value.Value)) {
                                    var bytes = new byte[reader.UnconsumedBufferLength];
                                    reader.ReadBytes(bytes);
                                    item.device_information = System.Text.Encoding.UTF8.GetString(bytes);
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
    }
}
