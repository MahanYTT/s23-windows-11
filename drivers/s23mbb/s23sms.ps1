<#
.SYNOPSIS
    Native Windows 11 SMS client for Samsung Galaxy S23 (s23mbb.sys / WwanSvc / mbnapi.dll).

.DESCRIPTION
    Connects to Windows' built-in Mobile Broadband COM API (IMbnInterfaceManager -> IMbnSms)
    backed by WwanSvc -> mbbcx.sys -> s23mbb.sys (MBIM UUID_SMS <-> QMI_WMS 0x05).
    Run without arguments to open the native WinForms SMS UI, or pass -Check for a
    non-interactive CLI smoke test.
#>
[CmdletBinding()]
param(
    [switch]$Check,
    [switch]$CompileOnly
)

if (-not $Check -and -not $CompileOnly -and [System.Threading.Thread]::CurrentThread.ApartmentState -ne [System.Threading.ApartmentState]::STA) {
    $argsList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-STA', '-File', $PSCommandPath)
    Start-Process -FilePath 'powershell.exe' -ArgumentList $argsList | Out-Null
    return
}

Add-Type -ReferencedAssemblies System.Windows.Forms, System.Drawing -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace S23Sms {
    public enum MBN_READY_STATE {
        OFF = 0,
        INITIALIZED = 1,
        SIM_NOT_INSERTED = 2,
        BAD_SIM = 3,
        FAILURE = 4,
        NOT_ACTIVATED = 5,
        DEVICE_LOCKED = 6,
        NO_ESIM_PROFILE = 7
    }

    public enum MBN_SMS_FLAG {
        ALL = 0,
        INDEX = 1,
        NEW = 2,
        OLD = 3,
        SENT = 4,
        DRAFT = 5
    }

    public enum MBN_SMS_FORMAT {
        NONE = 0,
        PDU = 1,
        TEXT = 2
    }

    public enum MBN_MSG_STATUS {
        NEW = 0,
        OLD = 1,
        DRAFT = 2,
        SENT = 3
    }

    public enum MBN_SMS_STATUS_FLAG {
        NONE = 0,
        MESSAGE_STORE_FULL = 1,
        NEW_MESSAGE = 2
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MBN_SMS_FILTER {
        public uint flag;
        public uint messageIndex;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MBN_SMS_STATUS_INFO {
        public uint flag;
        public uint messageIndex;
    }

    [ComImport, Guid("BDFEE05B-4418-11DD-90ED-001C257CCFF1")]
    public class MbnInterfaceManagerClass {
    }

    [ComImport, Guid("DCBBBAB6-201B-4BBB-AAEE-338E368AF6FA"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IMbnInterfaceManager {
        [return: MarshalAs(UnmanagedType.Interface)]
        object GetInterface([MarshalAs(UnmanagedType.LPWStr)] string interfaceID);
        [return: MarshalAs(UnmanagedType.SafeArray, SafeArraySubType = VarEnum.VT_UNKNOWN)]
        object[] GetInterfaces();
    }

    [ComImport, Guid("DCBBBAB6-2001-4BBB-AAEE-338E368AF6FA"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IMbnInterface {
        [return: MarshalAs(UnmanagedType.BStr)]
        string get_InterfaceID();
        void GetInterfaceCapability_Unused(IntPtr caps);
        [return: MarshalAs(UnmanagedType.Interface)]
        object GetSubscriberInformation();
        void GetReadyState(out int readyState);
    }

    [ComImport, Guid("DCBBBAB6-2012-4BBB-AAEE-338E368AF6FA"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IMbnSmsConfiguration {
        [return: MarshalAs(UnmanagedType.BStr)]
        string get_ServiceCenterAddress();
        void put_ServiceCenterAddress([MarshalAs(UnmanagedType.LPWStr)] string scAddress);
        uint get_MaxMessageIndex();
        uint get_CdmaShortMsgSize();
        int get_SmsFormat();
        void put_SmsFormat(int smsFormat);
    }

    [ComImport, Guid("DCBBBAB6-2013-4BBB-AAEE-338E368AF6FA"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IMbnSmsReadMsgPdu {
        uint get_Index();
        int get_Status();
        [return: MarshalAs(UnmanagedType.BStr)]
        string get_PduData();
        [return: MarshalAs(UnmanagedType.SafeArray, SafeArraySubType = VarEnum.VT_UI1)]
        byte[] get_Message();
    }

    [ComImport, Guid("DCBBBAB6-2015-4BBB-AAEE-338E368AF6FA"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IMbnSms {
        IMbnSmsConfiguration GetSmsConfiguration();
        void SetSmsConfiguration(IMbnSmsConfiguration smsConfiguration, out uint requestID);
        void SmsSendPdu([MarshalAs(UnmanagedType.LPWStr)] string pduData, byte size, out uint requestID);
        void SmsSendCdma_Unused([MarshalAs(UnmanagedType.LPWStr)] string address, int encoding, int language, uint sizeInCharacters, [MarshalAs(UnmanagedType.SafeArray, SafeArraySubType = VarEnum.VT_UI1)] byte[] message, out uint requestID);
        void SmsSendCdmaPdu_Unused([MarshalAs(UnmanagedType.SafeArray, SafeArraySubType = VarEnum.VT_UI1)] byte[] message, out uint requestID);
        void SmsRead(ref MBN_SMS_FILTER smsFilter, int smsFormat, out uint requestID);
        void SmsDelete(ref MBN_SMS_FILTER smsFilter, out uint requestID);
        void GetSmsStatus(out MBN_SMS_STATUS_INFO smsStatusInfo);
    }

    [ComImport, Guid("DCBBBAB6-2016-4BBB-AAEE-338E368AF6FA"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IMbnSmsEvents {
        void OnSmsConfigurationChange(IMbnSms sms);
        void OnSetSmsConfigurationComplete(IMbnSms sms, uint requestID, int status);
        void OnSmsSendComplete(IMbnSms sms, uint requestID, int status);
        void OnSmsReadComplete(
            IMbnSms sms,
            int smsFormat,
            [MarshalAs(UnmanagedType.SafeArray, SafeArraySubType = VarEnum.VT_UNKNOWN)] object[] readMsgs,
            [MarshalAs(UnmanagedType.VariantBool)] bool moreMsgs,
            uint requestID,
            int status);
        void OnSmsNewClass0Message(
            IMbnSms sms,
            int smsFormat,
            [MarshalAs(UnmanagedType.SafeArray, SafeArraySubType = VarEnum.VT_UNKNOWN)] object[] readMsgs);
        void OnSmsDeleteComplete(IMbnSms sms, uint requestID, int status);
        void OnSmsStatusChange(IMbnSms sms);
    }

    public class DecodedSms {
        public uint Index;
        public int Status;
        public string StatusName;
        public string Address;
        public string Timestamp;
        public string Body;
        public string RawHex;
        public string Summary;
    }

    public static class SmsPduCodec {
        private const string Gsm7Default =
            "@\u00A3$\u00A5\u00E8\u00E9\u00F9\u00EC\u00F2\u00C7\n\u00D8\u00F8\r\u00C5\u00E5" +
            "\u0394_\u03A6\u0393\u039B\u03A9\u03A0\u03A8\u03A3\u0398\u039E\u001B\u00C6\u00E6\u00DF\u00C9" +
            " !\"#\u00A4%&'()*+,-./" +
            "0123456789:;<=>?" +
            "\u00A1ABCDEFGHIJKLMNOPQRSTUVWXYZ\u00C4\u00D6\u00D1\u00DC\u00A7" +
            "\u00BFabcdefghijklmnopqrstuvwxyz\u00E4\u00F6\u00F1\u00FC\u00E0";

        private static int FindGsm7Code(char ch, out bool escaped) {
            escaped = false;
            if (ch == '\u20AC') { escaped = true; return 0x65; }
            switch (ch) {
                case '\f': escaped = true; return 0x0A;
                case '^':  escaped = true; return 0x14;
                case '{':  escaped = true; return 0x28;
                case '}':  escaped = true; return 0x29;
                case '\\': escaped = true; return 0x2F;
                case '[':  escaped = true; return 0x3C;
                case '~':  escaped = true; return 0x3D;
                case ']':  escaped = true; return 0x3E;
                case '|':  escaped = true; return 0x40;
            }
            int idx = Gsm7Default.IndexOf(ch);
            return (idx >= 0 && idx != 0x1B) ? idx : -1;
        }

        private static char MapGsm7Ext(byte b) {
            switch (b) {
                case 0x0A: return '\f';
                case 0x14: return '^';
                case 0x28: return '{';
                case 0x29: return '}';
                case 0x2F: return '\\';
                case 0x3C: return '[';
                case 0x3D: return '~';
                case 0x3E: return ']';
                case 0x40: return '|';
                case 0x65: return '\u20AC';
                default: return ' ';
            }
        }

        public static byte[] HexToBytes(string hex) {
            if (string.IsNullOrEmpty(hex)) return new byte[0];
            string clean = hex.Trim().Replace(" ", "");
            int len = clean.Length / 2;
            byte[] outBytes = new byte[len];
            for (int i = 0; i < len; i++) {
                outBytes[i] = byte.Parse(clean.Substring(i * 2, 2), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
            }
            return outBytes;
        }

        public static string BytesToHex(byte[] data, int offset, int count) {
            StringBuilder sb = new StringBuilder(count * 2);
            for (int i = 0; i < count; i++) {
                sb.AppendFormat("{0:X2}", data[offset + i]);
            }
            return sb.ToString();
        }

        private static string DecodeGsm7(byte[] data, int offset, int availBytes, int septets, int skipSeptets) {
            StringBuilder sb = new StringBuilder();
            bool esc = false;
            for (int i = 0; i < septets; i++) {
                int bit = i * 7;
                int bIdx = offset + (bit >> 3);
                int sh = bit & 7;
                if (bIdx >= offset + availBytes) break;
                int val = (data[bIdx] >> sh) & 0x7F;
                if (sh > 1 && (bIdx + 1) < (offset + availBytes)) {
                    val |= (data[bIdx + 1] << (8 - sh)) & 0x7F;
                }
                if (i < skipSeptets) continue;
                byte sep = (byte)val;
                if (esc) {
                    sb.Append(MapGsm7Ext(sep));
                    esc = false;
                } else if (sep == 0x1B) {
                    esc = true;
                } else {
                    sb.Append(sep < Gsm7Default.Length ? Gsm7Default[sep] : '?');
                }
            }
            return sb.ToString();
        }

        private static string DecodeAddress(byte[] data, int offset, int digits, byte tonNpi) {
            int octets = (digits + 1) / 2;
            if ((tonNpi & 0x70) == 0x50) {
                int septets = (digits * 4) / 7;
                return DecodeGsm7(data, offset, octets, septets, 0);
            }
            StringBuilder sb = new StringBuilder();
            if ((tonNpi & 0x70) == 0x10) sb.Append('+');
            for (int i = 0; i < octets; i++) {
                byte b = data[offset + i];
                int lo = b & 0x0F;
                int hi = (b >> 4) & 0x0F;
                if (lo <= 9) sb.Append((char)('0' + lo));
                else if (lo == 10) sb.Append('*');
                else if (lo == 11) sb.Append('#');
                if (hi <= 9) sb.Append((char)('0' + hi));
                else if (hi == 10) sb.Append('*');
                else if (hi == 11) sb.Append('#');
            }
            return sb.ToString();
        }

        public static string StatusToName(int status) {
            switch (status) {
                case (int)MBN_MSG_STATUS.NEW: return "Unread";
                case (int)MBN_MSG_STATUS.OLD: return "Read";
                case (int)MBN_MSG_STATUS.DRAFT: return "Draft";
                case (int)MBN_MSG_STATUS.SENT: return "Sent";
                default: return "Status(" + status + ")";
            }
        }

        public static DecodedSms Decode(uint index, int status, string pduHex, byte[] rawBytes) {
            DecodedSms res = new DecodedSms();
            res.Index = index;
            res.Status = status;
            res.StatusName = StatusToName(status);
            res.Address = "(unknown)";
            res.Timestamp = "-";
            res.Body = "";

            byte[] pdu = (rawBytes != null && rawBytes.Length > 0) ? rawBytes : HexToBytes(pduHex);
            res.RawHex = string.IsNullOrEmpty(pduHex) ? BytesToHex(pdu, 0, pdu.Length) : pduHex;
            if (pdu.Length < 4) {
                res.Body = "[Empty/Invalid PDU: " + res.RawHex + "]";
                res.Summary = res.Body;
                return res;
            }

            bool hasSca = (pdu[0] == 0) || (pdu[0] <= 12 && (1 + pdu[0]) < pdu.Length && (pdu[1] & 0x80) == 0x80);
            int tpStart = hasSca ? (1 + pdu[0]) : 0;
            if (tpStart >= pdu.Length) {
                res.Body = "[Truncated SCA PDU: " + res.RawHex + "]";
                res.Summary = res.Body;
                return res;
            }

            int p = tpStart;
            byte first = pdu[p++];
            int mti = first & 0x03;
            bool udhi = (first & 0x40) != 0;
            if (mti == 1 && p < pdu.Length) p++; // TP-MR on SMS-SUBMIT

            if (p + 2 > pdu.Length) {
                res.Body = "[Truncated Address PDU: " + res.RawHex + "]";
                res.Summary = res.Body;
                return res;
            }
            int addrDigits = pdu[p++];
            byte addrTon = pdu[p++];
            int addrOctets = (addrDigits + 1) / 2;
            if (p + addrOctets + 2 > pdu.Length) {
                res.Body = "[Truncated Header PDU: " + res.RawHex + "]";
                res.Summary = res.Body;
                return res;
            }
            res.Address = DecodeAddress(pdu, p, addrDigits, addrTon);
            p += addrOctets;

            p++; // TP-PID
            byte dcs = pdu[p++];

            if (mti == 0 && p + 7 <= pdu.Length) {
                res.Timestamp = string.Format(
                    CultureInfo.InvariantCulture,
                    "20{0}{1}-{2}{3}-{4}{5} {6}{7}:{8}{9}:{10}{11}",
                    pdu[p] & 0x0F, (pdu[p] >> 4) & 0x0F,
                    pdu[p + 1] & 0x0F, (pdu[p + 1] >> 4) & 0x0F,
                    pdu[p + 2] & 0x0F, (pdu[p + 2] >> 4) & 0x0F,
                    pdu[p + 3] & 0x0F, (pdu[p + 3] >> 4) & 0x0F,
                    pdu[p + 4] & 0x0F, (pdu[p + 4] >> 4) & 0x0F,
                    pdu[p + 5] & 0x0F, (pdu[p + 5] >> 4) & 0x0F);
                p += 7;
            } else if (mti == 1) {
                int vpf = (first >> 3) & 0x03;
                if (vpf == 2 && p < pdu.Length) p += 1;
                else if ((vpf == 1 || vpf == 3) && p + 7 <= pdu.Length) p += 7;
            }

            if (p < pdu.Length) {
                int udl = pdu[p++];
                int udBytes = pdu.Length - p;
                int alphabet = (dcs & 0xC0) == 0 ? ((dcs >> 2) & 0x03) : (((dcs & 0xF0) == 0xF0) ? (((dcs >> 2) & 0x01) == 1 ? 1 : 0) : 0);
                int udhl = 0;
                if (udhi && udBytes > 0) {
                    udhl = 1 + pdu[p];
                    if (udhl > udBytes) udhl = udBytes;
                }

                if (alphabet == 2) {
                    int start = p + udhl;
                    int byteCount = Math.Max(0, Math.Min(udl - udhl, pdu.Length - start));
                    if ((byteCount & 1) == 1) byteCount--;
                    res.Body = Encoding.BigEndianUnicode.GetString(pdu, start, byteCount);
                } else if (alphabet == 1) {
                    int start = p + udhl;
                    int byteCount = Math.Max(0, Math.Min(udl - udhl, pdu.Length - start));
                    res.Body = "[8-bit data] " + BytesToHex(pdu, start, byteCount);
                } else {
                    int skipSeptets = udhl > 0 ? ((udhl * 8 + 6) / 7) : 0;
                    res.Body = DecodeGsm7(pdu, p, udBytes, udl, skipSeptets);
                }
            }
            res.Summary = res.Body.Replace("\r", " ").Replace("\n", " ");
            return res;
        }

        public static bool CanEncodeGsm7(string text, out List<byte> septets) {
            septets = new List<byte>();
            if (text == null) return true;
            foreach (char ch in text) {
                bool esc;
                int code = FindGsm7Code(ch, out esc);
                if (code < 0) return false;
                if (esc) septets.Add(0x1B);
                septets.Add((byte)code);
            }
            return true;
        }

        public static void EncodeSubmitPdu(string destPhone, string text, out string fullPduHex, out byte tpduSizeBytes) {
            string clean = (destPhone ?? "").Trim();
            bool intl = clean.StartsWith("+");
            StringBuilder digits = new StringBuilder();
            foreach (char c in clean) {
                if (c >= '0' && c <= '9') digits.Append(c);
            }
            if (digits.Length == 0 || digits.Length > 20) {
                throw new ArgumentException("Destination phone number must contain 1-20 digits.");
            }

            List<byte> tpdu = new List<byte>();
            tpdu.Add(0x11); // SMS-SUBMIT, VPF=relative (1 byte)
            tpdu.Add(0x00); // TP-MR (modem assigns)
            tpdu.Add((byte)digits.Length);
            tpdu.Add(intl ? (byte)0x91 : (byte)0x81);
            for (int i = 0; i < digits.Length; i += 2) {
                int d1 = digits[i] - '0';
                int d2 = (i + 1 < digits.Length) ? (digits[i + 1] - '0') : 0x0F;
                tpdu.Add((byte)(d1 | (d2 << 4)));
            }
            tpdu.Add(0x00); // TP-PID

            List<byte> septets;
            if (CanEncodeGsm7(text ?? "", out septets) && septets.Count <= 160) {
                tpdu.Add(0x00); // TP-DCS: GSM-7
                tpdu.Add(0xA7); // TP-VP: 24 hours
                tpdu.Add((byte)septets.Count);
                int packedLen = (septets.Count * 7 + 7) / 8;
                byte[] packed = new byte[packedLen];
                for (int i = 0; i < septets.Count; i++) {
                    int bit = i * 7;
                    int bi = bit >> 3;
                    int sh = bit & 7;
                    packed[bi] |= (byte)((septets[i] << sh) & 0xFF);
                    if (sh > 1) {
                        packed[bi + 1] |= (byte)(septets[i] >> (8 - sh));
                    }
                }
                tpdu.AddRange(packed);
            } else {
                tpdu.Add(0x08); // TP-DCS: UCS-2
                tpdu.Add(0xA7);
                byte[] utf16 = Encoding.BigEndianUnicode.GetBytes(text ?? "");
                int maxBytes = Math.Min(140, utf16.Length);
                if ((maxBytes & 1) == 1) maxBytes--;
                tpdu.Add((byte)maxBytes);
                for (int i = 0; i < maxBytes; i++) tpdu.Add(utf16[i]);
            }

            tpduSizeBytes = (byte)tpdu.Count;
            byte[] full = new byte[1 + tpdu.Count];
            full[0] = 0x00; // Empty SCA -> modem fills default SIM SMSC
            tpdu.CopyTo(full, 1);
            fullPduHex = BytesToHex(full, 0, full.Length);
        }
    }

    [ComVisible(true)]
    [ClassInterface(ClassInterfaceType.None)]
    public class SmsEventsSink : IMbnSmsEvents {
        public event Action<string> StatusMessage;
        public event Action<List<DecodedSms>, int> ReadCompleted;
        public event Action<List<DecodedSms>> NewClass0Message;
        public event Action<uint, int> SendCompleted;
        public event Action<uint, int> DeleteCompleted;
        public event Action<uint, int> SetConfigCompleted;
        public event Action StoreChanged;

        private static List<DecodedSms> DecodeArray(object[] readMsgs) {
            List<DecodedSms> list = new List<DecodedSms>();
            if (readMsgs == null) return list;
            foreach (object obj in readMsgs) {
                IMbnSmsReadMsgPdu pduObj = obj as IMbnSmsReadMsgPdu;
                if (pduObj == null) continue;
                uint idx = pduObj.get_Index();
                int st = pduObj.get_Status();
                string hex = "";
                try { hex = pduObj.get_PduData(); } catch { }
                byte[] msg = null;
                try { msg = pduObj.get_Message(); } catch { }
                list.Add(SmsPduCodec.Decode(idx, st, hex, msg));
            }
            return list;
        }

        public void OnSmsConfigurationChange(IMbnSms sms) {
            if (StatusMessage != null) StatusMessage("SMS configuration updated.");
            if (StoreChanged != null) StoreChanged();
        }

        public void OnSetSmsConfigurationComplete(IMbnSms sms, uint requestID, int status) {
            if (SetConfigCompleted != null) SetConfigCompleted(requestID, status);
        }

        public void OnSmsSendComplete(IMbnSms sms, uint requestID, int status) {
            if (SendCompleted != null) SendCompleted(requestID, status);
        }

        public void OnSmsReadComplete(IMbnSms sms, int smsFormat, object[] readMsgs, bool moreMsgs, uint requestID, int status) {
            List<DecodedSms> items = DecodeArray(readMsgs);
            if (ReadCompleted != null) ReadCompleted(items, status);
        }

        public void OnSmsNewClass0Message(IMbnSms sms, int smsFormat, object[] readMsgs) {
            List<DecodedSms> items = DecodeArray(readMsgs);
            if (NewClass0Message != null) NewClass0Message(items);
        }

        public void OnSmsDeleteComplete(IMbnSms sms, uint requestID, int status) {
            if (DeleteCompleted != null) DeleteCompleted(requestID, status);
        }

        public void OnSmsStatusChange(IMbnSms sms) {
            if (StatusMessage != null) StatusMessage("New SMS notification received.");
            if (StoreChanged != null) StoreChanged();
        }
    }

    public class MbnSmsClient : IDisposable {
        public IMbnInterfaceManager Manager { get; private set; }
        public IMbnInterface Interface { get; private set; }
        public IMbnSms Sms { get; private set; }
        public SmsEventsSink Events { get; private set; }
        public string InterfaceId { get; private set; }

        private IConnectionPoint _cp;
        private int _cookie;

        public MbnSmsClient() {
            Events = new SmsEventsSink();
            Manager = (IMbnInterfaceManager)new MbnInterfaceManagerClass();
            IConnectionPointContainer cpc = Manager as IConnectionPointContainer;
            if (cpc != null) {
                Guid g = typeof(IMbnSmsEvents).GUID;
                cpc.FindConnectionPoint(ref g, out _cp);
                if (_cp != null) {
                    _cp.Advise(Events, out _cookie);
                }
            }
            ConnectInterface();
        }

        public bool ConnectInterface() {
            Interface = null;
            Sms = null;
            InterfaceId = "(none)";
            object[] ifaces = Manager.GetInterfaces();
            if (ifaces == null || ifaces.Length == 0) return false;
            Interface = (IMbnInterface)ifaces[0];
            InterfaceId = Interface.get_InterfaceID();
            Sms = Interface as IMbnSms;
            return Sms != null;
        }

        public void Dispose() {
            if (_cp != null && _cookie != 0) {
                try { _cp.Unadvise(_cookie); } catch { }
                _cookie = 0;
            }
        }
    }

    public class S23SmsForm : Form {
        private MbnSmsClient _client;
        private Label _lblIface;
        private TextBox _txtSmsc;
        private Button _btnSaveSmsc;
        private Button _btnRefresh;
        private Button _btnDelete;
        private ListView _list;
        private TextBox _txtViewer;
        private TextBox _txtTo;
        private TextBox _txtBody;
        private Label _lblChars;
        private Button _btnSend;
        private StatusStrip _status;
        private ToolStripStatusLabel _statusText;
        private List<DecodedSms> _messages = new List<DecodedSms>();

        public S23SmsForm() {
            Text = "S23 Cellular SMS (WwanSvc / s23mbb)";
            Size = new Size(880, 620);
            MinimumSize = new Size(740, 500);
            StartPosition = FormStartPosition.CenterScreen;
            Font = new Font("Segoe UI", 9.5f);

            BuildUi();
            InitMbn();
        }

        private void BuildUi() {
            Panel topPanel = new Panel { Dock = DockStyle.Top, Height = 74, Padding = new Padding(10, 8, 10, 6) };
            _lblIface = new Label { Location = new Point(10, 8), AutoSize = true, Text = "Interface: connecting..." };
            Label lblSmsc = new Label { Location = new Point(10, 40), AutoSize = true, Text = "SMSC:" };
            _txtSmsc = new TextBox { Location = new Point(58, 37), Width = 200 };
            _btnSaveSmsc = new Button { Location = new Point(266, 35), Width = 92, Height = 26, Text = "Save SMSC" };
            _btnRefresh = new Button { Location = new Point(368, 35), Width = 92, Height = 26, Text = "Refresh Inbox" };
            _btnDelete = new Button { Location = new Point(468, 35), Width = 110, Height = 26, Text = "Delete Selected" };

            _btnSaveSmsc.Click += (s, e) => SaveSmsc();
            _btnRefresh.Click += (s, e) => RefreshAll();
            _btnDelete.Click += (s, e) => DeleteSelected();

            topPanel.Controls.AddRange(new Control[] { _lblIface, lblSmsc, _txtSmsc, _btnSaveSmsc, _btnRefresh, _btnDelete });

            SplitContainer mainSplit = new SplitContainer {
                Dock = DockStyle.Fill,
                Orientation = Orientation.Horizontal,
                SplitterDistance = 260
            };

            _list = new ListView {
                Dock = DockStyle.Fill,
                View = View.Details,
                FullRowSelect = true,
                GridLines = true,
                MultiSelect = false
            };
            _list.Columns.Add("#", 45);
            _list.Columns.Add("Status", 75);
            _list.Columns.Add("From / To", 150);
            _list.Columns.Add("Timestamp", 145);
            _list.Columns.Add("Message", 420);
            _list.SelectedIndexChanged += (s, e) => ShowSelectedMessage();
            mainSplit.Panel1.Controls.Add(_list);

            SplitContainer bottomSplit = new SplitContainer {
                Dock = DockStyle.Fill,
                Orientation = Orientation.Vertical,
                SplitterDistance = 420
            };

            _txtViewer = new TextBox {
                Dock = DockStyle.Fill,
                Multiline = true,
                ReadOnly = true,
                ScrollBars = ScrollBars.Vertical,
                BackColor = Color.White
            };
            bottomSplit.Panel1.Controls.Add(_txtViewer);

            Panel sendPanel = new Panel { Dock = DockStyle.Fill, Padding = new Padding(10) };
            Label lblCompose = new Label { Location = new Point(10, 6), AutoSize = true, Font = new Font("Segoe UI", 9.5f, FontStyle.Bold), Text = "New SMS" };
            Label lblTo = new Label { Location = new Point(10, 34), AutoSize = true, Text = "To:" };
            _txtTo = new TextBox { Location = new Point(42, 31), Width = 220 };
            _lblChars = new Label { Location = new Point(272, 34), AutoSize = true, Text = "GSM-7 0/160" };
            _txtBody = new TextBox { Location = new Point(10, 62), Width = 390, Height = 120, Multiline = true, ScrollBars = ScrollBars.Vertical, Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right };
            _txtBody.TextChanged += (s, e) => UpdateCharCount();
            _btnSend = new Button { Location = new Point(10, 190), Width = 120, Height = 30, Text = "Send SMS", Anchor = AnchorStyles.Bottom | AnchorStyles.Left };
            _btnSend.Click += (s, e) => SendSms();
            sendPanel.Controls.AddRange(new Control[] { lblCompose, lblTo, _txtTo, _lblChars, _txtBody, _btnSend });
            bottomSplit.Panel2.Controls.Add(sendPanel);

            mainSplit.Panel2.Controls.Add(bottomSplit);

            _status = new StatusStrip();
            _statusText = new ToolStripStatusLabel("Initializing...");
            _status.Items.Add(_statusText);

            Controls.Add(mainSplit);
            Controls.Add(topPanel);
            Controls.Add(_status);
        }

        private void OnUi(Action act) {
            if (IsDisposed) return;
            if (InvokeRequired) BeginInvoke(act);
            else act();
        }

        private void SetStatus(string msg) {
            OnUi(() => { _statusText.Text = msg; });
        }

        private void InitMbn() {
            try {
                _client = new MbnSmsClient();
                _client.Events.StatusMessage += msg => SetStatus(msg);
                _client.Events.StoreChanged += () => OnUi(() => RefreshAll());
                _client.Events.ReadCompleted += (items, hr) => OnUi(() => OnReadCompleted(items, hr));
                _client.Events.NewClass0Message += items => OnUi(() => OnClass0(items));
                _client.Events.SendCompleted += (reqId, hr) => OnUi(() => {
                    _btnSend.Enabled = true;
                    SetStatus(hr == 0 ? "SMS sent (req=" + reqId + ")." : string.Format("SMS send failed: 0x{0:X8}", hr));
                });
                _client.Events.DeleteCompleted += (reqId, hr) => OnUi(() => {
                    SetStatus(hr == 0 ? "SMS deleted." : string.Format("Delete failed: 0x{0:X8}", hr));
                    if (hr == 0) RefreshAll();
                });
                _client.Events.SetConfigCompleted += (reqId, hr) => OnUi(() => {
                    SetStatus(hr == 0 ? "SMSC updated." : string.Format("SMSC update failed: 0x{0:X8}", hr));
                });
                FormClosed += (s, e) => { if (_client != null) _client.Dispose(); };
                RefreshAll();
            } catch (Exception ex) {
                _lblIface.Text = "MBN Error: " + ex.Message;
                SetStatus("Failed to connect to WwanSvc / MbnInterfaceManager.");
            }
        }

        private void RefreshAll() {
            if (_client == null) return;
            if (!_client.ConnectInterface() || _client.Sms == null) {
                _lblIface.Text = "No cellular Mobile Broadband interface active.";
                SetStatus("Waiting for WwanSvc cellular interface...");
                return;
            }
            int ready = -1;
            try { _client.Interface.GetReadyState(out ready); } catch { }
            string readyName = Enum.IsDefined(typeof(MBN_READY_STATE), ready) ? ((MBN_READY_STATE)ready).ToString() : ready.ToString();

            string smsc = "-";
            uint maxIdx = 0;
            try {
                IMbnSmsConfiguration cfg = _client.Sms.GetSmsConfiguration();
                smsc = cfg.get_ServiceCenterAddress() ?? "";
                maxIdx = cfg.get_MaxMessageIndex();
                _txtSmsc.Text = smsc;
            } catch (Exception ex) {
                smsc = "N/A (" + ex.Message + ")";
            }

            _lblIface.Text = string.Format("Interface: {0}  |  SIM State: {1}  |  Max Store Slots: {2}",
                _client.InterfaceId, readyName, maxIdx);

            try {
                MBN_SMS_FILTER flt = new MBN_SMS_FILTER { flag = (uint)MBN_SMS_FLAG.ALL, messageIndex = 0 };
                uint reqId;
                _client.Sms.SmsRead(ref flt, (int)MBN_SMS_FORMAT.PDU, out reqId);
                SetStatus("Reading SMS store (req=" + reqId + ")...");
            } catch (Exception ex) {
                SetStatus("SmsRead failed: " + ex.Message);
            }
        }

        private void OnReadCompleted(List<DecodedSms> items, int hr) {
            if (hr != 0) {
                SetStatus(string.Format("SMS read error: 0x{0:X8}", hr));
                return;
            }
            _messages = items ?? new List<DecodedSms>();
            _list.BeginUpdate();
            _list.Items.Clear();
            foreach (DecodedSms m in _messages) {
                ListViewItem lvi = new ListViewItem(m.Index.ToString(CultureInfo.InvariantCulture));
                lvi.SubItems.Add(m.StatusName);
                lvi.SubItems.Add(m.Address);
                lvi.SubItems.Add(m.Timestamp);
                lvi.SubItems.Add(m.Summary);
                lvi.Tag = m;
                _list.Items.Add(lvi);
            }
            _list.EndUpdate();
            SetStatus(string.Format("Loaded {0} message(s).", _messages.Count));
        }

        private void OnClass0(List<DecodedSms> items) {
            if (items == null || items.Count == 0) return;
            StringBuilder sb = new StringBuilder();
            foreach (DecodedSms m in items) {
                sb.AppendLine("From: " + m.Address);
                sb.AppendLine(m.Body);
                sb.AppendLine();
            }
            SetStatus("Flash/Class 0 SMS received.");
            MessageBox.Show(this, sb.ToString(), "Incoming Class 0 SMS", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }

        private void ShowSelectedMessage() {
            if (_list.SelectedItems.Count == 0) return;
            DecodedSms m = _list.SelectedItems[0].Tag as DecodedSms;
            if (m == null) return;
            _txtViewer.Text = string.Format(
                "Index: #{0} ({1})\r\nFrom/To: {2}\r\nTime: {3}\r\n\r\n{4}\r\n\r\n--- Raw PDU ---\r\n{5}",
                m.Index, m.StatusName, m.Address, m.Timestamp, m.Body, m.RawHex);
        }

        private void UpdateCharCount() {
            List<byte> septets;
            if (SmsPduCodec.CanEncodeGsm7(_txtBody.Text, out septets)) {
                _lblChars.Text = string.Format("GSM-7 {0}/160", septets.Count);
            } else {
                _lblChars.Text = string.Format("UCS-2 {0}/70", _txtBody.Text.Length);
            }
        }

        private void SaveSmsc() {
            if (_client == null || _client.Sms == null) return;
            try {
                IMbnSmsConfiguration cfg = _client.Sms.GetSmsConfiguration();
                cfg.put_ServiceCenterAddress(_txtSmsc.Text.Trim());
                cfg.put_SmsFormat((int)MBN_SMS_FORMAT.PDU);
                uint reqId;
                _client.Sms.SetSmsConfiguration(cfg, out reqId);
                SetStatus("Saving SMSC (req=" + reqId + ")...");
            } catch (Exception ex) {
                SetStatus("Save SMSC failed: " + ex.Message);
            }
        }

        private void SendSms() {
            if (_client == null || _client.Sms == null) return;
            try {
                string pduHex;
                byte tpduLen;
                SmsPduCodec.EncodeSubmitPdu(_txtTo.Text, _txtBody.Text, out pduHex, out tpduLen);
                uint reqId;
                _btnSend.Enabled = false;
                _client.Sms.SmsSendPdu(pduHex, tpduLen, out reqId);
                SetStatus("Sending SMS (req=" + reqId + ")...");
            } catch (Exception ex) {
                _btnSend.Enabled = true;
                SetStatus("Send failed: " + ex.Message);
            }
        }

        private void DeleteSelected() {
            if (_client == null || _client.Sms == null || _list.SelectedItems.Count == 0) return;
            DecodedSms m = _list.SelectedItems[0].Tag as DecodedSms;
            if (m == null) return;
            try {
                MBN_SMS_FILTER flt = new MBN_SMS_FILTER { flag = (uint)MBN_SMS_FLAG.INDEX, messageIndex = m.Index };
                uint reqId;
                _client.Sms.SmsDelete(ref flt, out reqId);
                SetStatus("Deleting SMS #" + m.Index + " (req=" + reqId + ")...");
            } catch (Exception ex) {
                SetStatus("Delete failed: " + ex.Message);
            }
        }

        public static int RunCheck() {
            Console.WriteLine("=== S23 Native MBN SMS Check (mbnapi.dll / WwanSvc) ===");
            using (MbnSmsClient client = new MbnSmsClient()) {
                if (!client.ConnectInterface() || client.Sms == null) {
                    Console.WriteLine("[WARN] No active Mobile Broadband interface returned by IMbnInterfaceManager.");
                    return 0;
                }
                int ready = -1;
                try { client.Interface.GetReadyState(out ready); } catch { }
                string readyName = Enum.IsDefined(typeof(MBN_READY_STATE), ready) ? ((MBN_READY_STATE)ready).ToString() : ready.ToString();
                Console.WriteLine("InterfaceID : " + client.InterfaceId);
                Console.WriteLine("ReadyState  : " + readyName + " (" + ready + ")");

                try {
                    IMbnSmsConfiguration cfg = client.Sms.GetSmsConfiguration();
                    Console.WriteLine("SMSC        : '" + (cfg.get_ServiceCenterAddress() ?? "") + "'");
                    Console.WriteLine("MaxIndex    : " + cfg.get_MaxMessageIndex());
                    Console.WriteLine("SmsFormat   : " + cfg.get_SmsFormat());
                } catch (Exception ex) {
                    Console.WriteLine("ConfigInfo  : unavailable (" + ex.Message + ")");
                }

                try {
                    MBN_SMS_STATUS_INFO st;
                    client.Sms.GetSmsStatus(out st);
                    Console.WriteLine("SmsStatus   : flag=" + st.flag + " index=" + st.messageIndex);
                } catch (Exception ex) {
                    Console.WriteLine("SmsStatus   : unavailable (" + ex.Message + ")");
                }

                bool done = false;
                int readHr = 0;
                List<DecodedSms> msgs = new List<DecodedSms>();
                client.Events.ReadCompleted += (items, hr) => {
                    readHr = hr;
                    if (items != null) msgs = items;
                    done = true;
                };

                try {
                    MBN_SMS_FILTER flt = new MBN_SMS_FILTER { flag = (uint)MBN_SMS_FLAG.ALL, messageIndex = 0 };
                    uint reqId;
                    client.Sms.SmsRead(ref flt, (int)MBN_SMS_FORMAT.PDU, out reqId);
                    Console.WriteLine("SmsRead(ALL): req=" + reqId);
                    int waitMs = 0;
                    while (!done && waitMs < 4000) {
                        Application.DoEvents();
                        Thread.Sleep(25);
                        waitMs += 25;
                    }
                    if (done) {
                        Console.WriteLine("OnSmsReadComplete: hr=0x" + readHr.ToString("X8") + " count=" + msgs.Count);
                        foreach (DecodedSms m in msgs) {
                            Console.WriteLine("  #" + m.Index + " [" + m.StatusName + "] " + m.Address + " (" + m.Timestamp + "): " + m.Summary);
                        }
                    } else {
                        Console.WriteLine("OnSmsReadComplete: timed out after 4000 ms (expected if no SIM/storage ready).");
                    }
                } catch (Exception ex) {
                    Console.WriteLine("SmsRead     : " + ex.Message);
                }
            }
            return 0;
        }
    }
}
'@

if ($CompileOnly) {
    Write-Output 'S23Sms C# compilation succeeded.'
    return
}

if ($Check) {
    $code = [S23Sms.S23SmsForm]::RunCheck()
    exit $code
}

[System.Windows.Forms.Application]::EnableVisualStyles()
[System.Windows.Forms.Application]::Run([S23Sms.S23SmsForm]::new())
