// =====================================================================================
//  Outlast1VR_Launcher.cs  -  found-footage camcorder launcher for the Outlast 1 VR mod
//  VR mod made by StrikeZoneVR
//
//  Single C# 5 source file for the .NET Framework 4.x compiler (see build_launcher.cmd):
//    csc /target:winexe /reference:System.Windows.Forms.dll /reference:System.Drawing.dll
//
//  Everything you see and hear is generated at runtime: no game artwork is shipped and no
//  audio files are used. If the user's own Outlast install has a splash bitmap it is
//  re-processed into night vision at runtime; otherwise a procedural corridor is drawn.
//
//  The installer reproduces release\Outlast-VR.bat (backups, ini edits, launch, restore),
//  with one change for this build: [SystemSettings] Fullscreen=False.
// =====================================================================================

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: AssemblyTitle("Outlast 1 VR Launcher")]
[assembly: AssemblyDescription("VR mod made by StrikeZoneVR")]
[assembly: AssemblyProduct("Outlast 1 VR")]
[assembly: AssemblyVersion("1.0.0.0")]
[assembly: AssemblyFileVersion("1.0.0.0")]
[assembly: AssemblyInformationalVersion("Beta 1.0")]

namespace Outlast1VR
{
    // ---------------------------------------------------------------------------------
    //  Entry point and command line
    // ---------------------------------------------------------------------------------
    static class Program
    {
        // The release this launcher ships with: stamped under the title and shown in the window title.
        public const string Release = "BETA 1.1";

        [DllImport("user32.dll")] static extern bool SetProcessDPIAware();

        [STAThread]
        static int Main(string[] args)
        {
            Opts o = Opts.Parse(args);
            // Real pixels on a scaled display (125%, 150%...): the full-screen window then covers the monitor
            // exactly and the picture is scaled once, by the launcher, instead of again by Windows.
            if (!o.IsDevTool) { try { SetProcessDPIAware(); } catch { } }
            if (o.IsDevTool)
            {
                try { return DevTools.Run(o); }
                catch (Exception ex)
                {
                    try { File.WriteAllText(Path.Combine(AppDir(), "Outlast1VR_devtool_error.txt"), ex.ToString()); } catch { }
                    return 2;
                }
            }

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.ThreadException += delegate(object s, ThreadExceptionEventArgs e)
            {
                MessageBox.Show("Unexpected launcher error:\n\n" + e.Exception.Message, "Outlast VR", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            };
            Application.Run(new LauncherForm(o.GameDir != null ? Path.GetFullPath(o.GameDir) : AppDir(), o.Fullscreen, o.PerfLog));
            if (SelfDeleteOnExit) ScheduleSelfDelete();
            return 0;
        }

        // [UNINSTALL] set by UNINSTALL VR MOD: a hidden cmd deletes this exe two seconds after it has exited.
        public static volatile bool SelfDeleteOnExit;
        static void ScheduleSelfDelete()
        {
            try
            {
                ProcessStartInfo psi = new ProcessStartInfo("cmd.exe", "/c ping 127.0.0.1 -n 3 > nul & del /f /q \"" + Application.ExecutablePath + "\"");
                psi.UseShellExecute = false; psi.CreateNoWindow = true; psi.WindowStyle = ProcessWindowStyle.Hidden;
                Process.Start(psi);
            }
            catch { }
        }

        public static string AppDir()
        {
            return AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\', '/');
        }
    }

    sealed class Opts
    {
        public string Preview, Bench, IniTest, RestoreTest, DumpAudio, WriteIcon, SysInfo, SelfTest, GameDir, SettingsTest;
        public int Frame = 150, Hover = 0, LaunchFrame = -1, ExitFrame = -1, ResX = 2560, ResY = 1440, Page = 0, PageSel = 0;
        public double PageAge = 1.6;
        public double HoverAge = 1.5;
        public bool Procedural, RestoreArmed, DryInstall;
        public int Fullscreen = -1;   // --fullscreen 1 / --windowed 0; -1 = the remembered choice
        public string PerfLog;        // --perf-log FILE: time the real window for 8 s (muted, nothing saved), then close

        public bool IsDevTool
        {
            get { return Preview != null || Bench != null || IniTest != null || RestoreTest != null || DumpAudio != null || WriteIcon != null || SysInfo != null || SelfTest != null || SettingsTest != null; }
        }

        public static Opts Parse(string[] a)
        {
            Opts o = new Opts();
            for (int i = 0; i < a.Length; i++)
            {
                string k = a[i].ToLowerInvariant();
                string next = i + 1 < a.Length ? a[i + 1] : null;
                switch (k)
                {
                    case "--preview": o.Preview = next; i++; break;
                    case "--frame": o.Frame = ToInt(next, 150); i++; break;
                    case "--hover": o.Hover = ToInt(next, 0); i++; break;
                    case "--hover-age": o.HoverAge = ToDouble(next, 1.5); i++; break;
                    case "--launch-frame": o.LaunchFrame = ToInt(next, 0); i++; break;
                    case "--exit-frame": o.ExitFrame = ToInt(next, 0); i++; break;
                    case "--procedural": o.Procedural = true; break;
                    case "--fullscreen": o.Fullscreen = 1; break;
                    case "--windowed": o.Fullscreen = 0; break;
                    case "--perf-log": o.PerfLog = next; i++; break;
                    case "--restore-armed": o.RestoreArmed = true; break;
                    case "--dry-install": o.DryInstall = true; break;
                    case "--page": o.Page = ToInt(next, 0); i++; break;
                    case "--page-sel": o.PageSel = ToInt(next, 0); i++; break;
                    case "--page-age": o.PageAge = ToDouble(next, 1.6); i++; break;
                    case "--gamedir": o.GameDir = next; i++; break;
                    case "--bench": o.Bench = next; i++; break;
                    case "--ini-test": o.IniTest = next; i++; break;
                    case "--restore-test": o.RestoreTest = next; i++; break;
                    case "--dump-audio": o.DumpAudio = next; i++; break;
                    case "--write-icon": o.WriteIcon = next; i++; break;
                    case "--sysinfo": o.SysInfo = next; i++; break;
                    case "--selftest": o.SelfTest = next; i++; break;
                    case "--settings-test": o.SettingsTest = next; i++; break;
                    case "--res":
                        if (next != null)
                        {
                            string[] p = next.ToLowerInvariant().Split('x');
                            if (p.Length == 2) { o.ResX = ToInt(p[0], 2560); o.ResY = ToInt(p[1], 1440); }
                        }
                        i++; break;
                }
            }
            return o;
        }

        static int ToInt(string s, int d) { int v; return int.TryParse(s, NumberStyles.Integer, CultureInfo.InvariantCulture, out v) ? v : d; }
        static double ToDouble(string s, double d) { double v; return double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out v) ? v : d; }
    }

    // ---------------------------------------------------------------------------------
    //  Small helpers
    // ---------------------------------------------------------------------------------
    static class U
    {
        public static double Smooth(double a, double b, double x)
        {
            if (x <= a) return 0; if (x >= b) return 1;
            double t = (x - a) / (b - a); return t * t * (3 - 2 * t);
        }

        public static double Clamp01(double v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

        public static double Hash(int a, int b)
        {
            unchecked
            {
                uint h = (uint)a * 0x9E3779B1u ^ ((uint)b + 0x7F4A7C15u) * 0x85EBCA77u;
                h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
                return (h & 0xFFFFFF) / 16777216.0;
            }
        }

        public static Color A(Color c, double a)
        {
            int v = (int)(c.A * a); if (v < 0) v = 0; if (v > 255) v = 255;
            return Color.FromArgb(v, c.R, c.G, c.B);
        }

        public static Font MakeFont(string[] families, float px, FontStyle st)
        {
            foreach (string f in families)
            {
                try
                {
                    Font font = new Font(f, px, st, GraphicsUnit.Pixel);
                    if (string.Equals(font.FontFamily.Name, f, StringComparison.OrdinalIgnoreCase)) return font;
                    font.Dispose();
                }
                catch { }
            }
            return new Font(FontFamily.GenericMonospace, px, st, GraphicsUnit.Pixel);
        }

        public static FontFamily Family(string[] families)
        {
            foreach (string f in families)
            {
                try { FontFamily ff = new FontFamily(f); return ff; } catch { }
            }
            return FontFamily.GenericMonospace;
        }

        public static readonly string[] Mono = { "Consolas", "Lucida Console", "Courier New" };
        public static readonly string[] Type = { "Courier New", "Consolas" };

        public static void SavePng(Bitmap b, string path)
        {
            using (MemoryStream ms = new MemoryStream())
            {
                b.Save(ms, ImageFormat.Png);
                File.WriteAllBytes(Path.GetFullPath(path), ms.ToArray());
            }
        }

        public static int[] ReadPixels(Bitmap b)
        {
            Rectangle r = new Rectangle(0, 0, b.Width, b.Height);
            BitmapData d = b.LockBits(r, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            int[] px = new int[b.Width * b.Height];
            try
            {
                if (d.Stride == b.Width * 4) Marshal.Copy(d.Scan0, px, 0, px.Length);
                else for (int y = 0; y < b.Height; y++) Marshal.Copy(new IntPtr(d.Scan0.ToInt64() + (long)y * d.Stride), px, y * b.Width, b.Width);
            }
            finally { b.UnlockBits(d); }
            return px;
        }

        public static Bitmap FromPixels(int[] px, int w, int h, PixelFormat fmt)
        {
            Bitmap b = new Bitmap(w, h, fmt);
            BitmapData d = b.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, fmt);
            try
            {
                if (d.Stride == w * 4) Marshal.Copy(px, 0, d.Scan0, px.Length);
                else for (int y = 0; y < h; y++) Marshal.Copy(px, y * w, new IntPtr(d.Scan0.ToInt64() + (long)y * d.Stride), w);
            }
            finally { b.UnlockBits(d); }
            return b;
        }
    }

    // ---------------------------------------------------------------------------------
    //  System information (paths, OpenXR runtime, mod build, physical desktop size)
    // ---------------------------------------------------------------------------------
    static class SysInfo
    {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct DEVMODE
        {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
            public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
            public int dmFields, dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
            public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
            public short dmLogPixels;
            public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
            public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
        }

        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool EnumDisplaySettings(string deviceName, int modeNum, ref DEVMODE devMode);
        [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr hwnd);
        [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);
        [DllImport("gdi32.dll")] static extern int GetDeviceCaps(IntPtr dc, int index);

        // (Test46e) Testers: "it is not applying the VR ini file" / "RUN OUTLAST ONCE FIRST" after they had run it.
        // Outlast's settings are not always where Windows says Documents is: OneDrive moves Documents (and the
        // game may have written them before or after), and started without Steam's -installed the game keeps
        // them in its own OLGame\Config. Every usual place is looked at; the most recently written OLGame.ini
        // wins. With none found it is the Documents one, as before.
        public static string GameDirForConfig;   // set by the launcher: Binaries\Win64
        public static string ConfigDir()
        {
            const string sub = @"My Games\Outlast\OLGame\Config";
            List<string> c = new List<string>();
            string docs = "";
            try { docs = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments); } catch { }
            if (docs.Length > 0) c.Add(Path.Combine(docs, sub));
            string home = Environment.GetEnvironmentVariable("USERPROFILE") ?? "";
            if (home.Length > 0)
            {
                c.Add(Path.Combine(home, @"Documents\" + sub));
                try { foreach (string od in Directory.GetDirectories(home, "OneDrive*")) c.Add(Path.Combine(od, @"Documents\" + sub)); } catch { }
            }
            string pub = Environment.GetEnvironmentVariable("PUBLIC") ?? "";
            if (pub.Length > 0) c.Add(Path.Combine(pub, @"Documents\" + sub));
            if (!string.IsNullOrEmpty(GameDirForConfig))
            {
                try { c.Add(Path.GetFullPath(Path.Combine(GameDirForConfig, @"..\..\OLGame\Config"))); } catch { }
            }
            string best = null; DateTime bestT = DateTime.MinValue;
            foreach (string d in c)
            {
                try
                {
                    string f = Path.Combine(d, "OLGame.ini");
                    if (!File.Exists(f)) continue;
                    DateTime t = File.GetLastWriteTimeUtc(f);
                    if (best == null || t > bestT) { best = d; bestT = t; }
                }
                catch { }
            }
            return best ?? (c.Count > 0 ? c[0] : Path.Combine(docs, sub));
        }

        // (Test46e) Launcher_Log.txt next to the mod's own log: every message the launcher shows, and every error
        // in full - for a tester to send.
        public static void FileLog(string line)
        {
            try
            {
                string d = LogDir(); Directory.CreateDirectory(d);
                File.AppendAllText(Path.Combine(d, "Launcher_Log.txt"), DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + "  " + line + "\r\n");
            }
            catch { }
        }

        public static string LogDir()
        {
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "OutlastVR");
        }

        static readonly Regex BuildRx = new Regex(@"(?<![A-Za-z_])BUILD=(.*)$", RegexOptions.IgnoreCase);

        public static string ReadBuild()
        {
            string p = Path.Combine(LogDir(), "OutlastVR_Log.txt");
            try
            {
                if (!File.Exists(p)) return null;
                // The game keeps this file open while it runs: share everything, read only.
                using (FileStream fs = new FileStream(p, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                using (StreamReader sr = new StreamReader(fs, Encoding.UTF8, true))
                {
                    string line; int n = 0;
                    while ((line = sr.ReadLine()) != null && n++ < 5000)
                    {
                        Match m = BuildRx.Match(line);
                        if (m.Success)
                        {
                            string v = m.Groups[1].Value.Trim();
                            int paren = v.IndexOf(" (", StringComparison.Ordinal);
                            if (paren > 0) v = v.Substring(0, paren).Trim();
                            return v.Length == 0 ? null : v;
                        }
                    }
                }
            }
            catch { }
            return null;
        }

        // The installed d3d9.dll carries its build tag as plain text ("BUILD=OUTLAST1VR-...").
        static readonly Regex DllBuildRx = new Regex(@"BUILD=(OUTLAST1VR-[A-Z0-9.\-]+)");

        public static string ReadDllBuild(string gameDir)
        {
            try
            {
                string p = Path.Combine(gameDir, "d3d9.dll");
                if (!File.Exists(p)) return null;
                Match m = DllBuildRx.Match(Encoding.ASCII.GetString(File.ReadAllBytes(p)));
                return m.Success ? m.Groups[1].Value : null;
            }
            catch { return null; }
        }

        // "OUTLAST1VR-BETA-1.0-20261014" -> "BETA 1.0  (2026-10-14)"; any other tag as it is.
        public static string FriendlyBuild(string tag)
        {
            if (tag == null) return null;
            Match m = Regex.Match(tag, @"-(BETA|RELEASE)-(\d+(?:\.\d+)*)-(\d{4})(\d{2})(\d{2})$", RegexOptions.IgnoreCase);
            if (!m.Success) return tag;
            return m.Groups[1].Value.ToUpperInvariant() + " " + m.Groups[2].Value + "  (" + m.Groups[3].Value + "-" + m.Groups[4].Value + "-" + m.Groups[5].Value + ")";
        }

        public static string ActiveRuntime()
        {
            RegistryView[] views = { RegistryView.Registry64, RegistryView.Registry32 };
            foreach (RegistryView view in views)
            {
                try
                {
                    using (RegistryKey b = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view))
                    using (RegistryKey k = b.OpenSubKey(@"SOFTWARE\Khronos\OpenXR\1"))
                    {
                        if (k == null) continue;
                        string v = k.GetValue("ActiveRuntime") as string;
                        if (!string.IsNullOrEmpty(v)) return v;
                    }
                }
                catch { }
            }
            return null;
        }

        public static string FriendlyRuntime(string p)
        {
            if (string.IsNullOrEmpty(p)) return null;
            string l = p.ToLowerInvariant();
            if (l.Contains("virtualdesktop") || l.Contains("virtual desktop")) return "VIRTUAL DESKTOP";
            if (l.Contains("steamxr") || l.Contains("steamvr")) return "STEAMVR";
            if (l.Contains("oculus") || l.Contains("meta")) return "META / OCULUS";
            if (l.Contains("mixedreality")) return "OTHER (WINDOWS MIXED REALITY)";
            string f = "UNKNOWN";
            try { f = Path.GetFileNameWithoutExtension(p); } catch { }
            if (f.Length > 22) f = f.Substring(0, 22);
            return "OTHER (" + f.ToUpperInvariant() + ")";
        }

        // Physical size of the primary display, independent of Windows display scaling
        // (the .bat does SetProcessDPIAware + Screen.PrimaryScreen.Bounds for the same value).
        public static Size PhysicalPrimary()
        {
            try
            {
                DEVMODE dm = new DEVMODE();
                dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
                if (EnumDisplaySettings(Screen.PrimaryScreen.DeviceName, -1, ref dm) && dm.dmPelsWidth > 0 && dm.dmPelsHeight > 0)
                    return new Size(dm.dmPelsWidth, dm.dmPelsHeight);
            }
            catch { }
            try
            {
                IntPtr dc = GetDC(IntPtr.Zero);
                int w = GetDeviceCaps(dc, 118), h = GetDeviceCaps(dc, 117);
                ReleaseDC(IntPtr.Zero, dc);
                if (w > 0 && h > 0) return new Size(w, h);
            }
            catch { }
            return Screen.PrimaryScreen.Bounds.Size;
        }

        public static string FindSplash(string gameDir)
        {
            if (string.IsNullOrEmpty(gameDir)) return null;
            string[] rel =
            {
                @"..\..\OLGame\Splash\PCConsole\Splash.bmp",
                @"..\..\OLGame\Splash\Splash.bmp",
                @"..\..\Engine\Splash\PCConsole\Splash.bmp",
                @"..\..\Engine\Splash\Splash.bmp",
            };
            foreach (string r in rel)
            {
                try
                {
                    string p = Path.GetFullPath(Path.Combine(gameDir, r));
                    if (File.Exists(p)) return p;
                }
                catch { }
            }
            return null;
        }

        public static bool GameRunning()
        {
            try { return Process.GetProcessesByName("OLGame").Length > 0; } catch { return false; }
        }
    }

    enum Tone { Ok, Warn, Err, Info }

    sealed class Msg
    {
        public string Text; public Tone Kind;
        public Msg(string text, Tone kind) { Text = text; Kind = kind; }
        public override string ToString() { return "[" + Kind + "] " + Text; }
    }

    sealed class Status
    {
        public string GameDir, ConfigDir, Build, Runtime, RuntimeName;
        public bool GameFound, D3d9, Loader, ConfigFound, Installed;

        public static Status Read(string gameDir)
        {
            Status s = new Status();
            s.GameDir = gameDir;
            try
            {
                s.GameFound = File.Exists(Path.Combine(gameDir, "OLGame.exe"));
                s.D3d9 = File.Exists(Path.Combine(gameDir, "d3d9.dll"));
                s.Loader = File.Exists(Path.Combine(gameDir, "openxr_loader.dll"));
            }
            catch { }
            try
            {
                s.ConfigDir = SysInfo.ConfigDir();
                s.ConfigFound = File.Exists(Path.Combine(s.ConfigDir, "OLGame.ini"));
                s.Installed = File.Exists(Path.Combine(s.ConfigDir, "OLGame.ini" + Installer.Bak));
            }
            catch { }
            s.Build = SysInfo.FriendlyBuild(SysInfo.ReadDllBuild(gameDir) ?? SysInfo.ReadBuild());
            s.Runtime = SysInfo.ActiveRuntime();
            s.RuntimeName = SysInfo.FriendlyRuntime(s.Runtime);
            return s;
        }
    }

    // ---------------------------------------------------------------------------------
    //  Installer: a faithful port of release\Outlast-VR.bat
    // ---------------------------------------------------------------------------------
    static class Installer
    {
        public const string Bak = ".bak-outlastvr";
        public static readonly string[] Files = { "OLGame.ini", "OLSystemSettings.ini", "OLEngine.ini" };

        // PowerShell -replace / -match are case-insensitive; so are these.
        const RegexOptions RX = RegexOptions.IgnoreCase | RegexOptions.CultureInvariant;

        // OLGame.ini  - wide FOV (the game renders about half the camera FOV per eye), pickup assist, no crosshair
        static readonly string[,] GameRules =
        {
            { "^DefaultFOV=.*", "DefaultFOV=120.0" },
            { "^RunningFOV=.*", "RunningFOV=126.0" },
            { "^CamcorderMaxFOV=.*", "CamcorderMaxFOV=120.0" },
            { "^CamcorderNVMaxFOV=.*", "CamcorderNVMaxFOV=120.0" },
            { "^NVLightZoomedInBrightness=.*", "NVLightZoomedInBrightness=0.015" },
            { "^MinCosAngleForPickup=.*", "MinCosAngleForPickup=0.70" },
            { "^PickupInteractRadius=.*", "PickupInteractRadius=200.0" },
            { "^bShowCrosshair=.*", "bShowCrosshair=false" },
        };

        // OLEngine.ini - only the stock values are changed
        static readonly string[,] EngineRules =
        {
            { @"^MaxSmoothedFrameRate=62(\.0+)?$", "MaxSmoothedFrameRate=90" },
            { "^PoolSize=160$", "PoolSize=1000" },
        };

        static Msg Ok(string s) { return new Msg(s, Tone.Ok); }
        static Msg Warn(string s) { return new Msg(s, Tone.Warn); }
        static Msg Err(string s) { return new Msg(s, Tone.Err); }
        static Msg Info(string s) { return new Msg(s, Tone.Info); }

        static string Short(Exception ex)
        {
            SysInfo.FileLog("ERROR " + ex.GetType().Name + " 0x" + ex.HResult.ToString("X8") + ": " + ex.Message);
            // (Test46e) a tester could not read the cut-off message ("BACKUP FAILED ... something to do with the
            // ini"): the common causes now read as what they are; the whole error goes to Launcher_Log.txt
            int hr = ex.HResult & 0xFFFF;
            // (Test46e) Controlled folder access (and some antivirus) answers a blocked copy/write in Documents with
            // FILE NOT FOUND for the file the launcher has just checked is there: "Could not find file ...OLGame.ini"
            if (ex is UnauthorizedAccessException || hr == 5 || ex is FileNotFoundException || hr == 2 || hr == 3) return "WINDOWS BLOCKED IT";
            if (hr == 0x20 || hr == 0x21) return "FILE IN USE - CLOSE OUTLAST";
            if (hr == 0x16A || hr == 0x179 || hr == 0x187) return "ONEDRIVE IS NOT RUNNING";
            if (hr == 0x70) return "DISK FULL";
            string m = ex.Message ?? ex.GetType().Name;
            m = m.Replace("\r", " ").Replace("\n", " ");
            return m.Length > 40 ? m.Substring(0, 40) + "..." : m;
        }
        // What to do about it, one short line.
        static Msg Fix(Exception ex)
        {
            int hr = ex.HResult & 0xFFFF;
            if (ex is UnauthorizedAccessException || hr == 5 || ex is FileNotFoundException || hr == 2 || hr == 3)
                return Warn("ALLOW Outlast1VR.exe IN RANSOMWARE PROTECTION");
            if (hr == 0x16A || hr == 0x179 || hr == 0x187) return Warn("START ONEDRIVE, THEN TRY AGAIN");
            return Warn("SEE %LOCALAPPDATA%\\OutlastVR\\Launcher_Log.txt");
        }

        public static int ApplyGame(List<string> lines)
        {
            int hits = 0;
            for (int i = 0; i < lines.Count; i++)
            {
                string l = lines[i]; bool hit = false;
                for (int k = 0; k < GameRules.GetLength(0); k++)
                {
                    if (Regex.IsMatch(l, GameRules[k, 0], RX)) hit = true;
                    l = Regex.Replace(l, GameRules[k, 0], GameRules[k, 1], RX);
                }
                if (hit) hits++;
                lines[i] = l;
            }
            return hits;
        }

        // The [SystemSettings] block, plus motion blur off and 16x filtering in the quality presets:
        // the game re-applies its preset (SystemSettingsBucketN) at every start and saves it over
        // [SystemSettings] on exit, which silently turned motion blur back on. Same as the bat.
        public static int ApplySystem(List<string> lines, int resX, int resY)
        {
            string section = ""; int hits = 0;
            for (int i = 0; i < lines.Count; i++)
            {
                string l = lines[i];
                Match m = Regex.Match(l, @"^\[(.+)\]", RX);
                if (m.Success) section = m.Groups[1].Value;
                if (section.StartsWith("SystemSettingsBucket", StringComparison.OrdinalIgnoreCase))
                {
                    if (Regex.IsMatch(l, "^MotionBlur=", RX)) l = "MotionBlur=False";
                    else if (Regex.IsMatch(l, "^MotionBlurPause=", RX)) l = "MotionBlurPause=False";
                    else if (Regex.IsMatch(l, "^MaxAnisotropy=", RX)) l = "MaxAnisotropy=16";
                    else continue;
                    hits++;
                    lines[i] = l;
                    continue;
                }
                if (!string.Equals(section, "SystemSettings", StringComparison.OrdinalIgnoreCase)) continue;
                string o = l;
                if (Regex.IsMatch(l, "^DepthOfField=", RX)) l = "DepthOfField=True";
                else if (Regex.IsMatch(l, "^MotionBlur=", RX)) l = "MotionBlur=False";
                else if (Regex.IsMatch(l, "^MotionBlurPause=", RX)) l = "MotionBlurPause=False";
                else if (Regex.IsMatch(l, "^UpscaleScreenPercentage=", RX)) l = "UpscaleScreenPercentage=False";
                else if (Regex.IsMatch(l, "^Fullscreen=", RX)) l = "Fullscreen=False";   // this build: windowed for the high-res renderer
                else if (Regex.IsMatch(l, "^ResX=", RX)) l = "ResX=" + resX.ToString(CultureInfo.InvariantCulture);
                else if (Regex.IsMatch(l, "^ResY=", RX)) l = "ResY=" + resY.ToString(CultureInfo.InvariantCulture);
                else if (Regex.IsMatch(l, "^ScreenPercentage=", RX)) l = "ScreenPercentage=100.000000";
                else if (Regex.IsMatch(l, "^bAllowPostprocessFXAA=", RX)) l = "bAllowPostprocessFXAA=False";
                else if (Regex.IsMatch(l, "^MaxAnisotropy=", RX)) l = "MaxAnisotropy=16";
                else if (Regex.IsMatch(l, "^TEXTUREGROUP_", RX)) l = Regex.Replace(l, "MipFilter=Point", "MipFilter=Linear", RX);
                else continue;
                hits++;
                lines[i] = l;
                if (o == l) { }
            }
            return hits;
        }

        public static void ApplyEngine(List<string> lines, out bool fps, out bool pool)
        {
            fps = false; pool = false;
            for (int i = 0; i < lines.Count; i++)
            {
                string l = lines[i];
                if (Regex.IsMatch(l, EngineRules[0, 0], RX)) fps = true;
                if (Regex.IsMatch(l, EngineRules[1, 0], RX)) pool = true;
                for (int k = 0; k < EngineRules.GetLength(0); k++) l = Regex.Replace(l, EngineRules[k, 0], EngineRules[k, 1], RX);
                lines[i] = l;
            }
        }

        // Get-Content semantics: BOM detection, otherwise the ANSI code page; lines split on CR, LF or CRLF.
        public static List<string> SplitLines(byte[] data)
        {
            List<string> lines = new List<string>();
            using (StreamReader sr = new StreamReader(new MemoryStream(data), Encoding.Default, true))
            {
                string l;
                while ((l = sr.ReadLine()) != null) lines.Add(l);
            }
            return lines;
        }

        // Windows PowerShell 5.1 Set-Content semantics: ANSI code page, no BOM, CRLF after every line.
        public static byte[] JoinLines(List<string> lines)
        {
            StringBuilder sb = new StringBuilder();
            foreach (string l in lines) sb.Append(l).Append("\r\n");
            return Encoding.Default.GetBytes(sb.ToString());
        }

        static byte[] ReadShared(string path)
        {
            using (FileStream fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            using (MemoryStream ms = new MemoryStream())
            {
                fs.CopyTo(ms);
                return ms.ToArray();
            }
        }

        static bool Same(byte[] a, byte[] b)
        {
            if (a.Length != b.Length) return false;
            for (int i = 0; i < a.Length; i++) if (a[i] != b[i]) return false;
            return true;
        }

        static void WriteSafe(string path, byte[] data)
        {
            // (Test46e) a settings file marked read-only (some players lock their settings) made every write fail
            try { FileAttributes a = File.GetAttributes(path); if ((a & FileAttributes.ReadOnly) != 0) File.SetAttributes(path, a & ~FileAttributes.ReadOnly); } catch { }
            string tmp = path + ".tmp-outlastvr";
            File.WriteAllBytes(tmp, data);
            File.Copy(tmp, path, true);
            try { File.Delete(tmp); } catch { }
        }

        delegate int LineEdit(List<string> lines);

        static bool EditFile(string path, string name, bool dry, LineEdit edit, List<Msg> r, out int hits, out bool found)
        {
            hits = 0; found = false;
            if (!File.Exists(path)) { r.Add(Warn(name + " NOT FOUND - SKIPPED")); return true; }
            found = true;
            try
            {
                byte[] old = ReadShared(path);
                List<string> lines = SplitLines(old);
                if (lines.Count == 0) { r.Add(Warn(name + " IS EMPTY - SKIPPED")); return true; }
                hits = edit(lines);
                byte[] nu = JoinLines(lines);
                if (!Same(old, nu) && !dry) WriteSafe(path, nu);
                return true;
            }
            catch (Exception ex)
            {
                SysInfo.FileLog("could not update " + path);
                r.Add(Err(name + " NOT UPDATED: " + Short(ex)));
                r.Add(Fix(ex));
                return false;
            }
        }

        public static bool Install(string cfg, int resX, int resY, List<Msg> r, bool dry)
        {
            string gameIni = Path.Combine(cfg, "OLGame.ini");
            if (!File.Exists(gameIni)) { r.Add(Err("RUN OUTLAST ONCE FIRST (NO OLGame.ini YET)")); return false; }

            // Backups only if none exist yet, so the first (original) copy is never overwritten.
            int made = 0, kept = 0;
            foreach (string f in Files)
            {
                string src = Path.Combine(cfg, f), bak = src + Bak;
                try
                {
                    if (File.Exists(bak)) kept++;
                    else if (File.Exists(src)) { if (!dry) File.Copy(src, bak, true); made++; }
                }
                catch (Exception ex)
                {
                    SysInfo.FileLog("backup of " + src + " to " + bak + " failed");
                    r.Add(Err("BACKUP FAILED: " + f + " - " + Short(ex)));
                    r.Add(Fix(ex));
                    r.Add(Err("NOTHING WAS CHANGED"));
                    return false;
                }
            }
            if (made > 0 && kept > 0) r.Add(Ok("BACKUPS: " + made + " SAVED, " + kept + " KEPT (.bak-outlastvr)"));
            else if (made > 0) r.Add(Ok("BACKUPS: " + made + " ORIGINAL INI FILES SAVED"));
            else r.Add(Ok("BACKUPS: ORIGINALS ALREADY SAVED - KEPT"));

            bool ok = true; int hits; bool found;

            ok &= EditFile(gameIni, "OLGame.ini", dry, ApplyGame, r, out hits, out found);
            if (found) r.Add(hits > 0 ? Ok("OLGame.ini: FOV 120/126 · PICKUP ASSIST · NO CROSSHAIR")
                                      : Warn("OLGame.ini: NO MATCHING KEYS - LEFT AS IS"));

            int rx = resX, ry = resY;
            ok &= EditFile(Path.Combine(cfg, "OLSystemSettings.ini"), "OLSystemSettings.ini", dry,
                           delegate(List<string> l) { return ApplySystem(l, rx, ry); }, r, out hits, out found);
            if (found) r.Add(hits > 0 ? Ok("SYSTEM: WINDOWED " + rx + "x" + ry + " · FXAA OFF · 16X AF")
                                      : Warn("OLSystemSettings.ini: NO [SystemSettings] KEYS"));

            bool fps = false, pool = false;
            ok &= EditFile(Path.Combine(cfg, "OLEngine.ini"), "OLEngine.ini", dry,
                           delegate(List<string> l) { ApplyEngine(l, out fps, out pool); return (fps ? 1 : 0) + (pool ? 1 : 0); }, r, out hits, out found);
            if (found) r.Add(Ok("OLEngine.ini: " + (fps ? "90 FPS CAP" : "FPS CAP ALREADY SET") + " · " + (pool ? "TEXTURE POOL 1000" : "POOL ALREADY SET")));

            if (ok) r.Add(Ok(dry ? "VR PROFILE CHECKED (PREVIEW - NOTHING WRITTEN)" : "SHARP VR PROFILE INSTALLED"));
            return ok;
        }

        // [UNINSTALL] (Test45) "People were confused on how to uninstall it." The button says UNINSTALL VR MOD and
        // takes the whole mod out of the game folder: the game's own settings back (when the mod saved them),
        // then every mod file. README.txt / TESTER NOTES.txt only when they are ours; the launcher removes itself
        // after it closes (Program.SelfDeleteOnExit). Before, with no saved settings it did nothing at all -
        // the DLLs stayed - and it always left the launcher, the ini, the .bat, the logo and the README.
        static readonly string[] ModFiles = { "d3d9.dll", "openxr_loader.dll", "outlastvr.ini", "Outlast-VR.bat", "strikezone_logo.jpg" };
        static readonly string[] ModTexts = { "README.txt", "TESTER NOTES.txt" };

        public static bool Restore(string cfg, string gameDir, List<Msg> r, bool dry)
        {
            // The game's settings first: leaving FOV 120 behind would break the normal game.
            bool ok = true; int restored = 0;
            foreach (string f in Files)
            {
                string ini = Path.Combine(cfg, f), bak = ini + Bak;
                if (!File.Exists(bak)) continue;
                try
                {
                    if (!dry) { File.Copy(bak, ini, true); File.Delete(bak); }
                    restored++;
                }
                catch (Exception ex) { ok = false; r.Add(Err("COULD NOT PUT BACK " + f + ": " + Short(ex))); }
            }
            if (restored > 0) r.Add(Ok("GAME SETTINGS PUT BACK (" + restored + " ORIGINAL INI FILE" + (restored == 1 ? "" : "S") + ")"));
            else r.Add(Info("NO SAVED GAME SETTINGS - LEFT AS THEY ARE"));

            int removed = 0;
            List<string> gone = new List<string>();
            foreach (string f in ModFiles) gone.Add(f);
            foreach (string f in ModTexts)
            {
                string p = Path.Combine(gameDir, f);
                try { if (File.Exists(p) && File.ReadAllText(p).IndexOf("OUTLAST VR", StringComparison.OrdinalIgnoreCase) >= 0) gone.Add(f); } catch { }
            }
            foreach (string f in gone)
            {
                string p = Path.Combine(gameDir, f);
                try
                {
                    if (File.Exists(p)) { if (!dry) File.Delete(p); removed++; }
                }
                catch (Exception ex) { ok = false; r.Add(Warn("COULD NOT DELETE " + f + ": " + Short(ex))); }
            }
            r.Add(Ok(removed > 0 ? "REMOVED " + removed + " VR MOD FILE" + (removed == 1 ? "" : "S") + " FROM THE GAME FOLDER" : "NO VR MOD FILES LEFT IN THE GAME FOLDER"));
            r.Add(Ok(dry ? "UNINSTALL CHECKED (PREVIEW - NOTHING CHANGED)" : "UNINSTALLED. THE NORMAL GAME IS BACK."));
            return ok;
        }
    }

    // ---------------------------------------------------------------------------------
    //  Procedural audio: synthesis
    // ---------------------------------------------------------------------------------
    static class Synth
    {
        public const int Rate = 22050;

        public static float[] Buf(double sec) { return new float[(int)(sec * Rate) + 1]; }

        public static void Thump(float[] b, double at, double f0, double f1, double tau, double amp)
        {
            int s = (int)(at * Rate), n = (int)(tau * 7 * Rate); double ph = 0;
            for (int i = 0; i < n && s + i < b.Length; i++)
            {
                if (s + i < 0) continue;
                double t = (double)i / Rate;
                double f = f1 + (f0 - f1) * Math.Exp(-t / (tau * 0.6));
                ph += 2 * Math.PI * f / Rate;
                double env = (1 - Math.Exp(-t / 0.0025)) * Math.Exp(-t / tau);
                b[s + i] += (float)(Math.Sin(ph) * env * amp);
            }
        }

        // Filtered noise. tau > 0: exponential decay after the attack, tau <= 0: sustained.
        public static void Noise(float[] b, Random r, double at, double dur, double attack, double tau, double amp, double lp, double hp, bool crackle)
        {
            int s = (int)(at * Rate), n = (int)(dur * Rate);
            double al = 1 - Math.Exp(-2 * Math.PI * lp / Rate), ah = 1 - Math.Exp(-2 * Math.PI * hp / Rate);
            double y1 = 0, y2 = 0, hs = 0, g = 1;
            double relLen = Math.Min(0.06, dur * 0.3);
            for (int i = 0; i < n && s + i < b.Length; i++)
            {
                double t = (double)i / Rate;
                double x = r.NextDouble() * 2 - 1;
                y1 += al * (x - y1); y2 += al * (y1 - y2);
                hs += ah * (y2 - hs);
                double v = y2 - hs;
                double env = t < attack ? t / attack : (tau > 0 ? Math.Exp(-(t - attack) / tau) : 1);
                double rel = (dur - t) / relLen; if (rel < 1) env *= Math.Max(0, rel);
                if (crackle)
                {
                    if (i % 260 == 0) g = 0.4 + 0.6 * r.NextDouble();
                    if (r.NextDouble() < 0.0009) v += (r.NextDouble() - 0.5) * 3;
                    env *= g;
                }
                if (s + i >= 0) b[s + i] += (float)(v * env * amp * 2.2);
            }
        }

        public static void Tone(float[] b, double at, double dur, double f, double amp, double h3, double h5)
        {
            int s = (int)(at * Rate), n = (int)(dur * Rate);
            for (int i = 0; i < n && s + i < b.Length; i++)
            {
                double t = (double)i / Rate, w = 2 * Math.PI * f * t;
                double env = Math.Min(1, Math.Min(t / 0.004, (dur - t) / 0.012));
                double v = Math.Sin(w) + h3 * Math.Sin(3 * w) + h5 * Math.Sin(5 * w);
                b[s + i] += (float)(v * env * amp);
            }
        }

        public static void Sweep(float[] b, double at, double dur, double f0, double f1, double amp)
        {
            int s = (int)(at * Rate), n = (int)(dur * Rate); double ph = 0;
            for (int i = 0; i < n && s + i < b.Length; i++)
            {
                double p = (double)i / n;
                double f = f0 * Math.Pow(f1 / f0, p);
                ph += 2 * Math.PI * f / Rate;
                double env = Math.Sqrt(Math.Sin(Math.PI * p));
                b[s + i] += (float)((Math.Sin(ph) + 0.25 * Math.Sin(2 * ph)) * env * amp);
            }
        }

        // Wooden creak: stick-slip impulses exciting two resonators.
        public static void Creak(float[] b, Random r, double at, double dur, double fres, double amp)
        {
            int s = (int)(at * Rate), n = (int)(dur * Rate);
            double y1 = 0, y2 = 0, z1 = 0, z2 = 0, lp = 0;
            int nextImp = 0;
            for (int i = 0; i < n && s + i < b.Length; i++)
            {
                double p = (double)i / n;
                double x = 0;
                if (i >= nextImp)
                {
                    x = 0.6 + 0.4 * r.NextDouble();
                    double interval = 0.007 + 0.016 * (0.5 + 0.5 * Math.Sin(p * 9.0 + r.NextDouble() * 0.6));
                    nextImp = i + (int)(interval * Rate);
                }
                double f = fres * (1 + 0.18 * Math.Sin(Math.PI * p));
                double w = 2 * Math.PI * f / Rate, R = 0.994;
                double y = x * (1 - R) * 6 + 2 * R * Math.Cos(w) * y1 - R * R * y2; y2 = y1; y1 = y;
                double w2 = 2 * Math.PI * f * 2.7 / Rate, R2 = 0.986;
                double z = x * (1 - R2) * 4 + 2 * R2 * Math.Cos(w2) * z1 - R2 * R2 * z2; z2 = z1; z1 = z;
                double v = y + 0.4 * z;
                lp += 0.35 * (v - lp);
                double env = Math.Pow(Math.Sin(Math.PI * p), 0.7);
                if (s + i >= 0) b[s + i] += (float)(lp * env * amp);
            }
        }

        // Small Schroeder reverb (4 damped combs) for distance.
        public static float[] Reverb(float[] dry, double wet)
        {
            double[] delays = { 0.0297, 0.0371, 0.0411, 0.0437 };
            float[] o = new float[dry.Length];
            foreach (double d in delays)
            {
                int D = (int)(d * Rate); float[] line = new float[D]; int idx = 0; double damp = 0;
                for (int i = 0; i < dry.Length; i++)
                {
                    double y = line[idx];
                    damp += 0.45 * (y - damp);
                    line[idx] = (float)(dry[i] + damp * 0.78);
                    idx = (idx + 1) % D;
                    o[i] += (float)(y * wet * 0.25);
                }
            }
            for (int i = 0; i < o.Length; i++) o[i] += dry[i];
            return o;
        }

        static void Peak(float[] b, double target)
        {
            double m = 1e-9; foreach (float v in b) m = Math.Max(m, Math.Abs(v));
            float k = (float)(target / m); for (int i = 0; i < b.Length; i++) b[i] *= k;
        }

        // ----- the actual sound effects -----

        public static float[] BootSequence()
        {
            Random r = new Random(1013);
            float[] b = Buf(1.85);
            // tape pushed into the camcorder: plastic clack + low thump
            Noise(b, r, 0.04, 0.03, 0.001, 0.006, 0.30, 5200, 1500, false);
            Thump(b, 0.05, 105, 55, 0.05, 0.42);
            // carriage slides down
            Noise(b, r, 0.12, 0.16, 0.06, 0, 0.06, 1500, 200, false);
            // lock clunk
            Noise(b, r, 0.30, 0.02, 0.001, 0.004, 0.26, 6000, 2000, false);
            Thump(b, 0.30, 125, 70, 0.04, 0.40);
            // static burst while the heads find the tracking
            Noise(b, r, 0.30, 0.68, 0.012, 0, 0.19, 6500, 380, true);
            for (int i = (int)(0.30 * Rate); i < (int)(0.98 * Rate) && i < b.Length; i++)
                b[i] += (float)(0.025 * Math.Sin(2 * Math.PI * 60 * i / Rate) * Math.Sin(2 * Math.PI * 7 * i / Rate));
            // motor spinning up
            Sweep(b, 0.36, 0.55, 90, 210, 0.035);
            // camcorder power-on: servo + beep
            Sweep(b, 0.98, 0.22, 480, 1050, 0.03);
            Tone(b, 1.12, 0.09, 2093, 0.12, 0.12, 0);
            // REC start: bip-bip
            Tone(b, 1.50, 0.05, 1568, 0.10, 0.1, 0);
            Tone(b, 1.58, 0.07, 2093, 0.10, 0.1, 0);
            return b;
        }

        public static float[] Ambient()
        {
            Random r = new Random(31);
            const double L = 32.0;                      // all tones have whole cycles in 32 s: seamless loop
            int N = (int)(L * Rate);
            float[] b = new float[N];
            for (int i = 0; i < N; i++)
            {
                double t = (double)i / Rate, tw = 2 * Math.PI * t;
                double hum = 0.030 * Math.Sin(tw * 60) + 0.020 * Math.Sin(tw * 120 + 0.3) + 0.012 * Math.Sin(tw * 180 + 1.1)
                           + 0.006 * Math.Sin(tw * 240 + 2.0) + 0.020 * Math.Sin(tw * 60.25 + 2.2);
                double drone = (0.020 * Math.Sin(tw * 73.4375) + 0.012 * Math.Sin(tw * 110.15625 + 0.7)) * (0.6 + 0.4 * Math.Sin(tw * 0.0625));
                b[i] = (float)(hum * (0.85 + 0.15 * Math.Sin(tw * 0.25)) + drone);
            }
            // air / distant wind, crossfaded so the loop point is invisible
            int X = Rate * 2;
            float[] air = new float[N + X];
            Noise(air, r, 0, (double)(N + X) / Rate, 0.01, 0, 0.05, 340, 40, false);
            for (int i = 0; i < air.Length; i++) air[i] *= (float)(0.55 + 0.45 * Math.Sin(2 * Math.PI * (double)i / Rate * 0.09375 + 1));
            for (int i = 0; i < N; i++)
            {
                double v = air[i];
                if (i < X) { double k = (double)i / X; v = air[i] * k + air[N + i] * (1 - k); }
                b[i] += (float)v;
            }
            // distant creaks and a far-away door, with a little room, tail folded back to the start
            float[] ev = new float[N + Rate * 3];
            Creak(ev, r, 6.2, 1.1, 340, 0.10);
            Creak(ev, r, 14.9, 0.8, 470, 0.07);
            Thump(ev, 23.4, 70, 38, 0.22, 0.20);
            Noise(ev, r, 23.4, 0.25, 0.002, 0.05, 0.05, 420, 60, false);
            Creak(ev, r, 28.1, 1.4, 260, 0.08);
            float[] rv = Reverb(ev, 0.9);
            for (int i = 0; i < rv.Length; i++) b[i % N] += rv[i] * 0.8f;
            Peak(b, 0.19);
            return b;
        }

        public static float[] Tick(int seed)
        {
            Random r = new Random(seed);
            float[] b = Buf(0.06);
            Noise(b, r, 0, 0.045, 0.0008, 0.007, 0.16, 7500, 2400, false);
            Tone(b, 0, 0.012, 2900 + seed * 37, 0.025, 0, 0);
            return b;
        }

        public static float[] Confirm()
        {
            float[] b = Buf(0.22);
            Tone(b, 0, 0.06, 1760, 0.12, 0.15, 0.05);
            Tone(b, 0.08, 0.10, 2349, 0.12, 0.15, 0.05);
            return b;
        }

        public static float[] Success()
        {
            float[] b = Buf(0.36);
            Tone(b, 0.00, 0.07, 1568, 0.09, 0.1, 0);
            Tone(b, 0.09, 0.07, 2093, 0.09, 0.1, 0);
            Tone(b, 0.18, 0.14, 2637, 0.09, 0.1, 0);
            return b;
        }

        public static float[] Error()
        {
            float[] b = Buf(0.42);
            Tone(b, 0.00, 0.13, 196, 0.10, 0.33, 0.2);
            Tone(b, 0.18, 0.18, 185, 0.10, 0.33, 0.2);
            return b;
        }

        public static float[] Armed()
        {
            float[] b = Buf(0.3);
            Tone(b, 0.00, 0.08, 880, 0.09, 0.3, 0.1);
            Tone(b, 0.12, 0.08, 880, 0.09, 0.3, 0.1);
            return b;
        }

        public static float[] Heartbeat()
        {
            Random r = new Random(66);
            float[] b = Buf(2.6);
            double[] beats = { 0.05, 0.90, 1.62 };
            foreach (double at in beats)
            {
                Thump(b, at, 64, 40, 0.075, 0.62);
                Noise(b, r, at, 0.06, 0.003, 0.02, 0.05, 260, 30, false);
                Thump(b, at + 0.24, 72, 46, 0.06, 0.40);
            }
            // signal switching to the headset: static swell + power-down whine + click
            Noise(b, r, 1.95, 0.42, 0.18, 0, 0.07, 5200, 400, true);
            Sweep(b, 1.95, 0.42, 1400, 90, 0.05);
            Noise(b, r, 2.38, 0.02, 0.001, 0.004, 0.2, 5000, 1500, false);
            Thump(b, 2.38, 85, 50, 0.03, 0.28);
            return b;
        }

        // Interference while something shows up on the tape: static swell, a low drop and a thin whine.
        public static float[] GhostCue()
        {
            Random r = new Random(13);
            float[] b = Buf(1.6);
            Noise(b, r, 0.0, 1.15, 0.22, 0, 0.07, 5200, 300, true);
            Thump(b, 0.25, 58, 30, 0.35, 0.20);
            Sweep(b, 0.2, 0.9, 2600, 2300, 0.012);
            return Reverb(b, 0.6);
        }

        public static float[] PowerOff()
        {
            Random r = new Random(5);
            float[] b = Buf(0.55);
            Sweep(b, 0, 0.36, 1250, 80, 0.05);
            Noise(b, r, 0, 0.3, 0.05, 0, 0.04, 4000, 500, true);
            Noise(b, r, 0.37, 0.02, 0.001, 0.004, 0.2, 5000, 1500, false);
            Thump(b, 0.37, 90, 50, 0.03, 0.26);
            return b;
        }

        public static byte[] ToWav(float[] s)
        {
            MemoryStream ms = new MemoryStream();
            BinaryWriter w = new BinaryWriter(ms);
            w.Write(Encoding.ASCII.GetBytes("RIFF")); w.Write(36 + s.Length * 2);
            w.Write(Encoding.ASCII.GetBytes("WAVEfmt ")); w.Write(16); w.Write((short)1); w.Write((short)1);
            w.Write(Rate); w.Write(Rate * 2); w.Write((short)2); w.Write((short)16);
            w.Write(Encoding.ASCII.GetBytes("data")); w.Write(s.Length * 2);
            foreach (float v in s) w.Write((short)(Math.Max(-1, Math.Min(1, v)) * 32000));
            w.Flush();
            return ms.ToArray();
        }
    }

    // ---------------------------------------------------------------------------------
    //  Procedural audio: a tiny waveOut mixer (overlapping sounds, loops, fades, mute)
    // ---------------------------------------------------------------------------------
    sealed class Mixer : IDisposable
    {
        [StructLayout(LayoutKind.Sequential, Pack = 2)]
        struct WAVEFORMATEX { public ushort wFormatTag, nChannels; public uint nSamplesPerSec, nAvgBytesPerSec; public ushort nBlockAlign, wBitsPerSample, cbSize; }

        [StructLayout(LayoutKind.Sequential)]
        struct WAVEHDR { public IntPtr lpData; public uint dwBufferLength, dwBytesRecorded; public IntPtr dwUser; public uint dwFlags, dwLoops; public IntPtr lpNext, reserved; }

        [DllImport("winmm.dll")] static extern int waveOutOpen(out IntPtr hwo, uint dev, ref WAVEFORMATEX fmt, IntPtr cb, IntPtr inst, uint flags);
        [DllImport("winmm.dll")] static extern int waveOutPrepareHeader(IntPtr hwo, IntPtr hdr, uint size);
        [DllImport("winmm.dll")] static extern int waveOutUnprepareHeader(IntPtr hwo, IntPtr hdr, uint size);
        [DllImport("winmm.dll")] static extern int waveOutWrite(IntPtr hwo, IntPtr hdr, uint size);
        [DllImport("winmm.dll")] static extern int waveOutReset(IntPtr hwo);
        [DllImport("winmm.dll")] static extern int waveOutClose(IntPtr hwo);

        const uint WAVE_MAPPER = 0xFFFFFFFF, CALLBACK_EVENT = 0x00050000, WHDR_DONE = 1;
        const int NB = 4, BS = 1024;

        sealed class Voice { public float[] D; public int P, Id; public float G, F, FT, FS; public bool Loop; }

        readonly List<Voice> voices = new List<Voice>();
        readonly object lk = new object();
        readonly AutoResetEvent ev = new AutoResetEvent(false);
        readonly float[] mix = new float[BS];
        readonly short[] pcm = new short[BS];
        IntPtr hwo = IntPtr.Zero;
        IntPtr[] hdr, dat;
        Thread th;
        volatile bool run;
        float master = 1f, masterTarget = 1f;
        int nextId = 1;
        public float Volume = 0.85f;
        public bool Ok { get; private set; }

        public Mixer()
        {
            try
            {
                WAVEFORMATEX f = new WAVEFORMATEX();
                f.wFormatTag = 1; f.nChannels = 1; f.nSamplesPerSec = Synth.Rate; f.wBitsPerSample = 16;
                f.nBlockAlign = 2; f.nAvgBytesPerSec = Synth.Rate * 2; f.cbSize = 0;
                if (waveOutOpen(out hwo, WAVE_MAPPER, ref f, ev.SafeWaitHandle.DangerousGetHandle(), IntPtr.Zero, CALLBACK_EVENT) != 0) { hwo = IntPtr.Zero; return; }
                uint hs = (uint)Marshal.SizeOf(typeof(WAVEHDR));
                hdr = new IntPtr[NB]; dat = new IntPtr[NB];
                for (int i = 0; i < NB; i++)
                {
                    dat[i] = Marshal.AllocHGlobal(BS * 2);
                    hdr[i] = Marshal.AllocHGlobal((int)hs);
                    WAVEHDR h = new WAVEHDR(); h.lpData = dat[i]; h.dwBufferLength = BS * 2;
                    Marshal.StructureToPtr(h, hdr[i], false);
                    waveOutPrepareHeader(hwo, hdr[i], hs);
                }
                run = true;
                th = new Thread(Loop); th.IsBackground = true; th.Priority = ThreadPriority.AboveNormal; th.Name = "OutlastVR audio";
                th.Start();
                Ok = true;
            }
            catch { Ok = false; }
        }

        void Loop()
        {
            uint hs = (uint)Marshal.SizeOf(typeof(WAVEHDR));
            int flagsOff = Marshal.OffsetOf(typeof(WAVEHDR), "dwFlags").ToInt32();
            bool[] queued = new bool[NB];
            int next = 0;
            while (run)
            {
                while (run && (!queued[next] || (Marshal.ReadInt32(hdr[next], flagsOff) & WHDR_DONE) != 0))
                {
                    Fill();
                    Marshal.Copy(pcm, 0, dat[next], BS);
                    waveOutWrite(hwo, hdr[next], hs);
                    queued[next] = true;
                    next = (next + 1) % NB;
                }
                ev.WaitOne(20);
            }
        }

        void Fill()
        {
            Array.Clear(mix, 0, BS);
            lock (lk)
            {
                for (int vi = voices.Count - 1; vi >= 0; vi--)
                {
                    Voice v = voices[vi]; bool dead = false;
                    for (int n = 0; n < BS; n++)
                    {
                        if (v.P >= v.D.Length) { if (v.Loop) v.P = 0; else { dead = true; break; } }
                        if (v.F != v.FT) { v.F = v.F < v.FT ? Math.Min(v.FT, v.F + v.FS) : Math.Max(v.FT, v.F - v.FS); }
                        mix[n] += v.D[v.P++] * v.G * v.F;
                    }
                    if (v.FT <= 0 && v.F <= 0) dead = true;
                    if (dead) voices.RemoveAt(vi);
                }
            }
            float step = 1f / (0.03f * Synth.Rate);
            for (int n = 0; n < BS; n++)
            {
                if (master != masterTarget) master = master < masterTarget ? Math.Min(masterTarget, master + step) : Math.Max(masterTarget, master - step);
                float x = mix[n] * master * Volume;
                x = x * (27 + x * x) / (27 + 9 * x * x);          // soft clip
                if (x > 1) x = 1; if (x < -1) x = -1;
                pcm[n] = (short)(x * 32000);
            }
        }

        public int Play(float[] data, float gain, bool loop, double fadeIn)
        {
            if (!Ok || data == null) return 0;
            Voice v = new Voice(); v.D = data; v.G = gain; v.Loop = loop; v.FT = 1;
            if (fadeIn > 0) { v.F = 0; v.FS = (float)(1.0 / (fadeIn * Synth.Rate)); } else v.F = 1;
            lock (lk) { v.Id = nextId++; voices.Add(v); }
            return v.Id;
        }

        public void Stop(int id, double fade)
        {
            if (!Ok || id == 0) return;
            lock (lk)
            {
                foreach (Voice v in voices)
                    if (v.Id == id) { v.FT = 0; v.FS = (float)(1.0 / (Math.Max(0.005, fade) * Synth.Rate)); }
            }
        }

        public bool IsPlaying(int id)
        {
            lock (lk) { foreach (Voice v in voices) if (v.Id == id) return true; }
            return false;
        }

        public bool Muted { set { masterTarget = value ? 0f : 1f; } }

        public void Dispose()
        {
            if (!Ok) return;
            Ok = false; run = false; ev.Set();
            try { if (th != null) th.Join(400); } catch { }
            try
            {
                waveOutReset(hwo);
                uint hs = (uint)Marshal.SizeOf(typeof(WAVEHDR));
                for (int i = 0; i < NB; i++) waveOutUnprepareHeader(hwo, hdr[i], hs);
                waveOutClose(hwo);
                for (int i = 0; i < NB; i++) { Marshal.FreeHGlobal(hdr[i]); Marshal.FreeHGlobal(dat[i]); }
            }
            catch { }
        }
    }

    sealed class Sfx : IDisposable
    {
        readonly Mixer mx = new Mixer();
        float[] boot, ambient, confirm, success, error, armed, heartbeat, poweroff, ghostCue;
        float[][] ticks;
        volatile bool ready;
        int bootId, ambId;
        double lastTick;
        readonly Stopwatch sw = Stopwatch.StartNew();

        public Sfx(bool muted)
        {
            mx.Muted = muted;
            boot = Synth.BootSequence();
            ThreadPool.QueueUserWorkItem(delegate
            {
                try
                {
                    ticks = new float[][] { Synth.Tick(1), Synth.Tick(2), Synth.Tick(3) };
                    confirm = Synth.Confirm(); success = Synth.Success(); error = Synth.Error(); armed = Synth.Armed();
                    heartbeat = Synth.Heartbeat(); poweroff = Synth.PowerOff(); ghostCue = Synth.GhostCue();
                    ambient = Synth.Ambient();
                    ready = true;
                }
                catch { }
            });
        }

        public bool Ready { get { return ready; } }
        public bool Muted { set { mx.Muted = value; } }
        public void Boot() { bootId = mx.Play(boot, 1f, false, 0); }
        public void StopBoot() { mx.Stop(bootId, 0.12); }
        public bool StartAmbient() { if (!ready) return false; if (ambId == 0) ambId = mx.Play(ambient, 1f, true, 2.5); return true; }
        public void StopAmbient(double fade) { mx.Stop(ambId, fade); ambId = 0; }
        public void Tick()
        {
            if (!ready) return;
            double now = sw.Elapsed.TotalSeconds; if (now - lastTick < 0.045) return; lastTick = now;
            mx.Play(ticks[(int)(now * 1000) % 3], 1f, false, 0);
        }
        public void Confirm() { if (ready) mx.Play(confirm, 1f, false, 0); }
        public void Success() { if (ready) mx.Play(success, 1f, false, 0); }
        public void Error() { if (ready) mx.Play(error, 1f, false, 0); }
        public void Armed() { if (ready) mx.Play(armed, 1f, false, 0); }
        public void Heartbeat() { if (ready) mx.Play(heartbeat, 1f, false, 0); }
        public void PowerOff() { if (ready) mx.Play(poweroff, 1f, false, 0); }
        public void Ghost() { if (ready) mx.Play(ghostCue, 1f, false, 0); }
        public void Dispose() { mx.Dispose(); }
    }

    // ---------------------------------------------------------------------------------
    //  Procedural art (built once; nothing per-pixel happens per frame)
    // ---------------------------------------------------------------------------------
    static class Art
    {
        public const int BW = 1320, BH = 760, HW = 660, HH = 380;
        // corridor camera: half-res projection
        const double VPX = 452, VPY = 150, F = 330, XL = -1.1, XR = 1.3, YF = -1.55, YC = 1.25, ZN = 0.25, ZF = 26;
        const double FigZ = ZF + 1.5;

        static PointF P(double x, double y, double z) { return new PointF((float)(VPX + F * x / z), (float)(VPY - F * y / z)); }

        public static PointF FocusBg() { PointF p = P(0, -0.45, ZF); return new PointF(p.X * 2, p.Y * 2); }
        public static PointF EyeBg(int side) { PointF p = P(0.08 + side * 0.045, 0.14, FigZ); return new PointF(p.X * 2, p.Y * 2); }

        static int[] palette;

        static int Pal(double v)
        {
            if (palette == null)
            {
                double[][] stops =
                {
                    new double[] { 0.00, 2, 7, 4 },
                    new double[] { 0.22, 14, 44, 24 },
                    new double[] { 0.48, 40, 112, 62 },
                    new double[] { 0.74, 96, 196, 118 },
                    new double[] { 1.00, 196, 255, 204 },
                };
                int[] p = new int[1024];
                for (int i = 0; i < 1024; i++)
                {
                    double x = i / 1023.0; int k = 0;
                    while (k < stops.Length - 2 && x > stops[k + 1][0]) k++;
                    double t = (x - stops[k][0]) / (stops[k + 1][0] - stops[k][0]);
                    int r = (int)(stops[k][1] + (stops[k + 1][1] - stops[k][1]) * t);
                    int g = (int)(stops[k][2] + (stops[k + 1][2] - stops[k][2]) * t);
                    int b = (int)(stops[k][3] + (stops[k + 1][3] - stops[k][3]) * t);
                    p[i] = unchecked((int)0xFF000000) | (r << 16) | (g << 8) | b;
                }
                palette = p;
            }
            int idx = (int)(v * 1023); if (idx < 0) idx = 0; if (idx > 1023) idx = 1023;
            return palette[idx];
        }

        delegate void Painter(Graphics g);

        static float[] RenderGray(Painter paint)
        {
            using (Bitmap b = new Bitmap(HW, HH, PixelFormat.Format32bppArgb))
            {
                using (Graphics g = Graphics.FromImage(b))
                {
                    g.Clear(Color.Black);
                    g.SmoothingMode = SmoothingMode.AntiAlias;
                    paint(g);
                }
                int[] px = U.ReadPixels(b);
                float[] o = new float[px.Length];
                for (int i = 0; i < px.Length; i++) o[i] = ((px[i] >> 16) & 255) / 255f;
                return o;
            }
        }

        static Color Gray(double v) { int c = (int)(U.Clamp01(v) * 255); return Color.FromArgb(c, c, c); }

        static void Poly(Graphics g, double v, params PointF[] p) { using (SolidBrush b = new SolidBrush(Gray(v))) g.FillPolygon(b, p); }
        static void PolyA(Graphics g, int alpha, double v, params PointF[] p) { int c = (int)(U.Clamp01(v) * 255); using (SolidBrush b = new SolidBrush(Color.FromArgb(alpha, c, c, c))) g.FillPolygon(b, p); }
        static void Line(Graphics g, double v, float w, PointF a, PointF b) { using (Pen p = new Pen(Gray(v), w)) g.DrawLine(p, a, b); }

        static void WallQuad(Graphics g, double x, double y0, double y1, double z0, double z1, double v)
        {
            Poly(g, v, P(x, y0, z0), P(x, y1, z0), P(x, y1, z1), P(x, y0, z1));
        }

        static void DrawAlbedo(Graphics g)
        {
            Random r = new Random(77);
            g.Clear(Gray(0.62));
            // ceiling with panel seams
            Poly(g, 0.52, P(XL, YC, ZN), P(XR, YC, ZN), P(XR, YC, ZF), P(XL, YC, ZF));
            for (double z = 0.8; z < ZF; z += 1.2) Line(g, 0.36, 1f, P(XL, YC, z), P(XR, YC, z));
            // walls: plaster above, glazed tiles below the wainscot
            WallQuad(g, XL, -0.45, YC, ZN, ZF, 0.74); WallQuad(g, XL, YF, -0.45, ZN, ZF, 0.56);
            WallQuad(g, XR, -0.45, YC, ZN, ZF, 0.70); WallQuad(g, XR, YF, -0.45, ZN, ZF, 0.54);
            for (double z = 0.5; z < ZF; z += 0.3)
            {
                Line(g, 0.42, 1f, P(XL, YF, z), P(XL, -0.45, z));
                Line(g, 0.40, 1f, P(XR, YF, z), P(XR, -0.45, z));
            }
            // grime and water stains
            for (int i = 0; i < 70; i++)
            {
                bool left = r.NextDouble() < 0.5; double x = left ? XL : XR;
                double z = 0.6 + Math.Pow(r.NextDouble(), 1.6) * 18, y = -1.4 + r.NextDouble() * 2.4;
                double sz = 0.15 + r.NextDouble() * 0.6;
                PointF c = P(x, y, z); float s = (float)(F * sz / z);
                using (SolidBrush b = new SolidBrush(Color.FromArgb(25 + r.Next(45), 0, 0, 0))) g.FillEllipse(b, c.X - s * 0.5f, c.Y - s, s, s * 2.2f);
            }
            for (int i = 0; i < 24; i++)
            {
                bool left = r.NextDouble() < 0.5; double x = left ? XL : XR;
                double z = 0.8 + r.NextDouble() * 14, y0 = 0.6 + r.NextDouble() * 0.5, y1 = y0 - 0.4 - r.NextDouble() * 1.2;
                using (Pen p = new Pen(Color.FromArgb(70, 0, 0, 0), Math.Max(1f, (float)(F * 0.02 / z)))) g.DrawLine(p, P(x, y0, z), P(x, y1, z));
            }
            // floor: checker tiles + dirt
            int row = 0;
            for (double z = ZN; z < ZF; z += 0.6, row++)
                for (int k = 0; k < 4; k++)
                {
                    double x0 = XL + k * 0.6, x1 = x0 + 0.6;
                    bool dark = (row + k) % 2 == 0;
                    Poly(g, dark ? 0.30 : 0.55, P(x0, YF, z), P(x1, YF, z), P(x1, YF, z + 0.6), P(x0, YF, z + 0.6));
                }
            for (int i = 0; i < 40; i++)
            {
                double x = XL + r.NextDouble() * (XR - XL), z = 0.8 + r.NextDouble() * 15, s = 0.2 + r.NextDouble() * 0.7;
                PolyA(g, 40 + r.Next(50), 0, P(x - s, YF, z - s * 0.6), P(x + s, YF, z - s * 0.4), P(x + s * 0.8, YF, z + s * 0.7), P(x - s * 0.7, YF, z + s * 0.5));
            }
            // wainscot trim + skirting
            Line(g, 0.22, 2f, P(XL, -0.45, ZN), P(XL, -0.45, ZF)); Line(g, 0.22, 2f, P(XR, -0.45, ZN), P(XR, -0.45, ZF));
            WallQuad(g, XL, YF, YF + 0.12, ZN, ZF, 0.25); WallQuad(g, XR, YF, YF + 0.12, ZN, ZF, 0.25);
            // pipes along the right wall
            WallQuad(g, XR - 0.01, 1.03, 1.12, ZN, ZF, 0.92); WallQuad(g, XR - 0.01, 0.97, 1.03, ZN, ZF, 0.18);
            // doors (side, z0, z1, open)
            double[,] doors = { { -1, 2.6, 3.5, 1 }, { 1, 4.8, 5.7, 0 }, { -1, 7.2, 8.1, 0 }, { 1, 10.0, 10.9, 1 }, { -1, 12.5, 13.4, 1 }, { 1, 15.5, 16.4, 0 }, { -1, 18.0, 18.9, 0 } };
            for (int i = 0; i < doors.GetLength(0); i++)
            {
                double x = doors[i, 0] < 0 ? XL : XR, z0 = doors[i, 1], z1 = doors[i, 2];
                bool open = doors[i, 3] > 0;
                WallQuad(g, x, YF, 0.6, z0 - 0.08, z1 + 0.08, 0.88);                    // frame
                WallQuad(g, x, YF, 0.53, z0, z1, open ? 0.0 : 0.40);                   // leaf or dark opening
                if (!open) WallQuad(g, x, 0.0, 0.32, z0 + 0.3, z1 - 0.3, 0.05);         // observation slot
            }
            // ceiling light housings
            double[] lamps = { 2.2, 5.6, 9.0, 13.4, 19.0 };
            foreach (double zc in lamps) Poly(g, 0.30, P(-0.25, YC, zc - 0.45), P(0.35, YC, zc - 0.45), P(0.35, YC, zc + 0.45), P(-0.25, YC, zc + 0.45));
            // far end: doorway + exit sign housing
            Poly(g, 0.0, P(-0.42, 0.55, ZF), P(0.42, 0.55, ZF), P(0.42, YF, ZF), P(-0.42, YF, ZF));
            Poly(g, 0.85, P(-0.5, 0.62, ZF), P(0.5, 0.62, ZF), P(0.5, 0.55, ZF), P(-0.5, 0.55, ZF));
            // scattered papers
            for (int i = 0; i < 12; i++)
            {
                double x = XL + 0.3 + r.NextDouble() * 1.9, z = 1.2 + r.NextDouble() * 11, s = 0.13 + r.NextDouble() * 0.08, a = r.NextDouble() * Math.PI;
                double ca = Math.Cos(a) * s, sa = Math.Sin(a) * s;
                Poly(g, 0.92, P(x - ca, YF, z - sa), P(x + sa, YF, z - ca), P(x + ca, YF, z + sa), P(x - sa, YF, z + ca));
            }
            // abandoned wheelchair
            DrawWheelchair(g, 0.62, 6.4);
            // overturned chair near the left wall
            PointF c0 = P(-0.75, YF, 4.2); float u = (float)(F / 4.2);
            using (Pen p = new Pen(Gray(0.06), Math.Max(1.5f, u * 0.04f)))
            {
                g.DrawLine(p, c0.X, c0.Y - u * 0.05f, c0.X + u * 0.45f, c0.Y - u * 0.12f);
                g.DrawLine(p, c0.X + u * 0.05f, c0.Y - u * 0.05f, c0.X + u * 0.12f, c0.Y - u * 0.45f);
                g.DrawLine(p, c0.X + u * 0.40f, c0.Y - u * 0.12f, c0.X + u * 0.47f, c0.Y - u * 0.50f);
                g.DrawLine(p, c0.X + u * 0.12f, c0.Y - u * 0.45f, c0.X + u * 0.47f, c0.Y - u * 0.50f);
            }
        }

        static void DrawWheelchair(Graphics g, double x, double z)
        {
            PointF b = P(x, YF, z); float u = (float)(F / z);             // pixels per metre
            using (SolidBrush sh = new SolidBrush(Color.FromArgb(90, 0, 0, 0))) g.FillEllipse(sh, b.X - u * 0.45f, b.Y - u * 0.06f, u * 0.9f, u * 0.12f);
            using (Pen p = new Pen(Gray(0.05), Math.Max(1.2f, u * 0.035f)))
            using (Pen thin = new Pen(Gray(0.10), Math.Max(1f, u * 0.015f)))
            {
                float R = u * 0.31f; PointF wc = new PointF(b.X + u * 0.05f, b.Y - R);
                g.DrawEllipse(p, wc.X - R, wc.Y - R, R * 2, R * 2);
                for (int k = 0; k < 6; k++) { double a = k * Math.PI / 6; g.DrawLine(thin, wc.X - (float)Math.Cos(a) * R, wc.Y - (float)Math.Sin(a) * R, wc.X + (float)Math.Cos(a) * R, wc.Y + (float)Math.Sin(a) * R); }
                float cr = u * 0.07f; g.DrawEllipse(p, b.X - u * 0.36f - cr, b.Y - cr * 2, cr * 2, cr * 2);
                g.DrawLine(p, b.X - u * 0.30f, b.Y - u * 0.48f, b.X + u * 0.18f, b.Y - u * 0.48f);   // seat
                g.DrawLine(p, b.X + u * 0.18f, b.Y - u * 0.48f, b.X + u * 0.24f, b.Y - u * 0.95f);   // back
                g.DrawLine(p, b.X + u * 0.24f, b.Y - u * 0.95f, b.X + u * 0.36f, b.Y - u * 0.95f);   // handle
                g.DrawLine(p, b.X - u * 0.30f, b.Y - u * 0.48f, b.X - u * 0.38f, b.Y - u * 0.12f);   // leg rest
                g.DrawLine(p, b.X - u * 0.30f, b.Y - u * 0.70f, b.X + u * 0.18f, b.Y - u * 0.70f);   // arm rest
            }
        }

        static void DrawEmissive(Graphics g, bool lampOn)
        {
            // light from a room beyond the far doorway
            PointF a = P(-0.42, 0.55, ZF), c = P(0.42, YF, ZF);
            RectangleF door = new RectangleF(a.X, a.Y, c.X - a.X, c.Y - a.Y);
            using (LinearGradientBrush lb = new LinearGradientBrush(new RectangleF(door.X, door.Y - 1, door.Width, door.Height + 2), Gray(0.10), Gray(0.26), 90f))
                g.FillRectangle(lb, door);
            // the figure standing in it
            using (SolidBrush k = new SolidBrush(Color.Black))
            {
                PointF h = P(0.08, 0.14, FigZ); float u = (float)(F / FigZ);
                g.FillEllipse(k, h.X - u * 0.12f, h.Y - u * 0.13f, u * 0.24f, u * 0.27f);
                PointF[] body =
                {
                    P(-0.18, 0.00, FigZ), P(0.30, -0.02, FigZ), P(0.27, -0.85, FigZ), P(0.22, YF, FigZ),
                    P(-0.08, YF, FigZ), P(-0.12, -0.85, FigZ)
                };
                g.FillPolygon(k, body);
            }
            // exit sign above it
            Poly(g, 0.95, P(-0.2, 0.86, ZF), P(0.2, 0.86, ZF), P(0.2, 0.70, ZF), P(-0.2, 0.70, ZF));
            // light leaking from the half-open door on the right
            Poly(g, 0.30, P(XR, YF, 10.85), P(XR, 0.5, 10.85), P(XR, 0.5, 10.95), P(XR, YF, 10.95));
            if (lampOn)
            {
                PointF p0 = P(-0.25, YC - 0.01, 8.55), p1 = P(0.35, YC - 0.01, 9.45);
                Poly(g, 1.0, P(-0.25, YC, 8.55), P(0.35, YC, 8.55), P(0.35, YC, 9.45), P(-0.25, YC, 9.45));
                float cx = (p0.X + p1.X) / 2, cy = (p0.Y + p1.Y) / 2;
                using (GraphicsPath gp = new GraphicsPath())
                {
                    gp.AddEllipse(cx - 70, cy - 34, 140, 68);
                    using (PathGradientBrush pb = new PathGradientBrush(gp))
                    {
                        pb.CenterColor = Color.FromArgb(150, 255, 255, 255);
                        pb.SurroundColors = new[] { Color.FromArgb(0, 255, 255, 255) };
                        g.FillPath(pb, gp);
                    }
                }
            }
        }

        static float[] Light(float[] albedo, float[] emis, bool lampOn)
        {
            float[] L = new float[HW * HH];
            double lx = 0.05, ly = YC - 0.06, lz = 9.0;
            for (int y = 0; y < HH; y++)
            {
                for (int x = 0; x < HW; x++)
                {
                    int i = y * HW + x;
                    double dx = x + 0.5 - VPX, dy = VPY - (y + 0.5);
                    double z = 1e9; int surf = 5;
                    if (dx < -1e-6) { double zz = XL * F / dx; if (zz < z) { z = zz; surf = 1; } }
                    if (dx > 1e-6) { double zz = XR * F / dx; if (zz < z) { z = zz; surf = 2; } }
                    if (dy > 1e-6) { double zz = YC * F / dy; if (zz < z) { z = zz; surf = 3; } }
                    if (dy < -1e-6) { double zz = YF * F / dy; if (zz < z) { z = zz; surf = 4; } }
                    if (z > ZF) { z = ZF; surf = 5; }
                    double X = dx * z / F, Y = dy * z / F;
                    double len = Math.Sqrt(X * X + Y * Y + z * z);
                    double cos = surf <= 2 ? Math.Abs(X) / len : (surf <= 4 ? Math.Abs(Y) / len : z / len);
                    double I = 1.45 / (0.8 * Math.Pow(z, 0.9) + 0.3);
                    double cx = (x - 330) / 330.0, cy = (y - 200) / 330.0;
                    double cone = 0.42 + 0.58 * Math.Exp(-(cx * cx + cy * cy) / 0.32);
                    double lit = I * (0.22 + 0.78 * cos) * cone;
                    if (lampOn)
                    {
                        double vx = lx - X, vy = ly - Y, vz = lz - z; double d2 = vx * vx + vy * vy + vz * vz, d = Math.Sqrt(d2);
                        double nd = surf == 1 ? vx : surf == 2 ? -vx : surf == 3 ? -vy : surf == 4 ? vy : -vz;
                        if (nd > 0) lit += 1.25 * (nd / d) / (1 + d2 * 0.30);
                    }
                    L[i] = (float)(albedo[i] * lit + emis[i] * 1.1 + 0.035 * (1 - Math.Exp(-z / 10)));
                }
            }
            return L;
        }

        // Upsample (bilinear, gives the soft camcorder look), tone-map into night-vision green, bake grain,
        // and darken the left side where the menu sits.
        static Bitmap Finish(float[] L, int lw, int lh, int seed, double ghost)
        {
            Random r = new Random(seed);
            int[] px = new int[BW * BH];
            double sxK = (double)lw / BW, syK = (double)lh / BH;
            int ghostPx = 12;
            for (int y = 0; y < BH; y++)
            {
                double sy = (y + 0.5) * syK - 0.5; int y0 = (int)Math.Floor(sy); double fy = sy - y0;
                int ya = Math.Max(0, Math.Min(lh - 1, y0)), yb = Math.Max(0, Math.Min(lh - 1, y0 + 1));
                double ys = y - 20;
                for (int x = 0; x < BW; x++)
                {
                    double v = Sample(L, lw, ya, yb, fy, x * 1.0, sxK);
                    if (ghost > 0 && x >= ghostPx) v += ghost * Sample(L, lw, ya, yb, fy, x - ghostPx, sxK);
                    double xs = x - 20;
                    double dark = 0.26 + 0.74 * U.Smooth(300, 780, xs);
                    dark *= 0.80 + 0.20 * (1 - U.Smooth(560, 760, ys) * (1 - U.Smooth(700, 1000, xs)));
                    double t = (1 - Math.Exp(-v * 1.85)) * dark;
                    t += (r.NextDouble() - 0.5) * 0.04;
                    px[y * BW + x] = Pal(t);
                }
            }
            return U.FromPixels(px, BW, BH, PixelFormat.Format32bppPArgb);
        }

        static double Sample(float[] L, int lw, int ya, int yb, double fy, double x, double sxK)
        {
            double sx = (x + 0.5) * sxK - 0.5; int x0 = (int)Math.Floor(sx); double fx = sx - x0;
            int xa = Math.Max(0, Math.Min(lw - 1, x0)), xb = Math.Max(0, Math.Min(lw - 1, x0 + 1));
            double top = L[ya * lw + xa] * (1 - fx) + L[ya * lw + xb] * fx;
            double bot = L[yb * lw + xa] * (1 - fx) + L[yb * lw + xb] * fx;
            return top * (1 - fy) + bot * fy;
        }

        public static void Corridor(out Bitmap lit, out Bitmap dark)
        {
            float[] albedo = RenderGray(DrawAlbedo);
            float[] eOn = RenderGray(delegate(Graphics g) { DrawEmissive(g, true); });
            float[] eOff = RenderGray(delegate(Graphics g) { DrawEmissive(g, false); });
            lit = Finish(Light(albedo, eOn, true), HW, HH, 11, 0.10);
            dark = Finish(Light(albedo, eOff, false), HW, HH, 11, 0.10);
        }

        // The user's own splash bitmap (read from their install at runtime, never shipped), re-processed into
        // night vision with its dark areas transparent. It is laid over the footage now and then as a brief
        // double exposure, the way something shows up on a tape for a few frames.
        public static Bitmap Ghost(string path)
        {
            using (Image src = Image.FromFile(path))
            using (Bitmap tmp = new Bitmap(BW, BH, PixelFormat.Format32bppArgb))
            {
                if (src.Width < 64 || src.Height < 64) return null;
                using (Graphics g = Graphics.FromImage(tmp))
                {
                    g.Clear(Color.Black);
                    g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                    g.DrawImage(src, new Rectangle(0, 0, BW, BH));
                }
                int[] px = U.ReadPixels(tmp);
                Random r = new Random(23);
                for (int y = 0; y < BH; y++)
                    for (int x = 0; x < BW; x++)
                    {
                        int i = y * BW + x, c = px[i];
                        double lum = (0.30 * ((c >> 16) & 255) + 0.59 * ((c >> 8) & 255) + 0.11 * (c & 255)) / 255.0;
                        double l = Math.Pow(lum, 0.8) * 1.9;
                        double v = (1 - Math.Exp(-l * 1.85)) * (0.35 + 0.65 * U.Smooth(300, 780, x - 20)) + (r.NextDouble() - 0.5) * 0.05;
                        double a = U.Smooth(0.015, 0.18, lum) * (0.25 + 0.75 * U.Smooth(260, 700, x - 20));
                        int p = Pal(v), A = (int)(a * 255);
                        int R = ((p >> 16) & 255) * A / 255, G = ((p >> 8) & 255) * A / 255, B = (p & 255) * A / 255;
                        px[i] = (A << 24) | (R << 16) | (G << 8) | B;
                    }
                return U.FromPixels(px, BW, BH, PixelFormat.Format32bppPArgb);
            }
        }
        public static Bitmap Overlay(int w, int h)
        {
            int[] px = new int[w * h];
            for (int y = 0; y < h; y++)
            {
                double ny = (y - h / 2.0) / (h / 2.0);
                double scan = (y % 3 == 2) ? 0.17 : (y % 3 == 0 ? 0.04 : 0);
                for (int x = 0; x < w; x++)
                {
                    double nx = (x - w / 2.0) / (w / 2.0);
                    double r2 = nx * nx * 0.85 + ny * ny * 1.05;
                    double vig = U.Clamp01((r2 - 0.30) * 0.62); vig = vig * vig * (3 - 2 * vig) * 0.86;
                    double a = 1 - (1 - scan) * (1 - vig);
                    px[y * w + x] = ((int)(a * 255) << 24);     // premultiplied black
                }
            }
            return U.FromPixels(px, w, h, PixelFormat.Format32bppPArgb);
        }

        public static Bitmap NoiseTile(int seed, bool opaque, int zoom)
        {
            const int S = 256; Random r = new Random(seed);
            int[] px = new int[S * S];
            double prev = 0.5;
            for (int i = 0; i < px.Length; i++)
            {
                if (opaque)
                {
                    double v = r.NextDouble(); v = prev * 0.45 + v * 0.55; prev = v;
                    int g = (int)(v * 235); int rr = (int)(g * 0.72), b = (int)(g * 0.80);
                    px[i] = unchecked((int)0xFF000000) | (rr << 16) | (g << 8) | b;
                }
                else
                {
                    double v = r.NextDouble();
                    int a = (int)(Math.Pow(v, 3.2) * 44);
                    bool darkSpeck = r.NextDouble() < 0.35;
                    int g = darkSpeck ? 0 : 255;
                    int rr = darkSpeck ? 0 : 205, b = darkSpeck ? 0 : 215;
                    px[i] = (a << 24) | ((rr * a / 255) << 16) | ((g * a / 255) << 8) | (b * a / 255);
                }
            }
            if (!opaque)
            {
                for (int k = 0; k < 5; k++)
                {
                    int y = r.Next(S), x0 = r.Next(S), len = 10 + r.Next(60); int a = 40 + r.Next(60);
                    for (int x = 0; x < len; x++) px[y * S + (x0 + x) % S] = (a << 24) | ((210 * a / 255) << 16) | ((255 * a / 255) << 8) | (220 * a / 255);
                }
            }
            if (zoom <= 1) return U.FromPixels(px, S, S, PixelFormat.Format32bppPArgb);
            int Z = S * zoom; int[] big = new int[Z * Z];
            for (int y = 0; y < Z; y++) for (int x = 0; x < Z; x++) big[y * Z + x] = px[(y / zoom) * S + x / zoom];
            return U.FromPixels(big, Z, Z, PixelFormat.Format32bppPArgb);
        }

        public static Bitmap Fog(int seed, int h, int alphaMax)
        {
            Bitmap b = new Bitmap(1280, h, PixelFormat.Format32bppPArgb);
            Random r = new Random(seed);
            using (Graphics g = Graphics.FromImage(b))
            {
                g.Clear(Color.Transparent);
                g.SmoothingMode = SmoothingMode.AntiAlias;
                for (int i = 0; i < 46; i++)
                {
                    float w = 200 + r.Next(380), hh = 50 + r.Next(110);
                    float x = r.Next(1280), y = hh * 0.6f + (float)r.NextDouble() * (h - hh * 1.2f);
                    int a = alphaMax / 2 + r.Next(alphaMax / 2);
                    for (int wrap = -1; wrap <= 1; wrap++)
                    {
                        using (GraphicsPath p = new GraphicsPath())
                        {
                            p.AddEllipse(x - w / 2 + wrap * 1280, y - hh / 2, w, hh);
                            using (PathGradientBrush pb = new PathGradientBrush(p))
                            {
                                pb.CenterColor = Color.FromArgb(a, 175, 235, 185);
                                pb.SurroundColors = new[] { Color.FromArgb(0, 175, 235, 185) };
                                g.FillPath(pb, p);
                            }
                        }
                    }
                }
            }
            return b;
        }

        // Value noise for the distressed typewriter title.
        static double VNoise(double x, double y, int seed)
        {
            int xi = (int)Math.Floor(x), yi = (int)Math.Floor(y); double fx = x - xi, fy = y - yi;
            fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
            double a = U.Hash(xi + seed * 131, yi), b = U.Hash(xi + 1 + seed * 131, yi), c = U.Hash(xi + seed * 131, yi + 1), d = U.Hash(xi + 1 + seed * 131, yi + 1);
            return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
        }

        public const int TitleW = 700, TitleH = 176, GlowPad = 14;

        public static void Title(out Bitmap title, out Bitmap glow, out Bitmap red, out Bitmap cyan)
        {
            FontFamily fam = U.Family(U.Type);
            List<GraphicsPath> word = new List<GraphicsPath>();
            List<GraphicsPath> stamp = new List<GraphicsPath>();
            Random r = new Random(1013);
            float baseY = 108, cell = 66, x0 = 4;
            string w = "OUTLAST";
            for (int i = 0; i < w.Length; i++)
            {
                GraphicsPath p = new GraphicsPath();
                p.AddString(w[i].ToString(), fam, (int)FontStyle.Bold, 112f, PointF.Empty, StringFormat.GenericTypographic);
                RectangleF bb = p.GetBounds();
                float jy = (float)(r.NextDouble() * 6 - 3), rot = (float)(r.NextDouble() * 3.2 - 1.6);
                using (Matrix m = new Matrix())
                {
                    float cx = x0 + i * cell + cell / 2, cy = baseY - bb.Height / 2 + jy;
                    m.Translate(cx, cy); m.Rotate(rot); m.Translate(-(bb.X + bb.Width / 2), -(bb.Y + bb.Height / 2));
                    p.Transform(m);
                }
                word.Add(p);
            }
            // "VR" as a stamped label
            GraphicsPath vr = new GraphicsPath();
            vr.AddString("VR", fam, (int)FontStyle.Bold, 70f, PointF.Empty, StringFormat.GenericTypographic);
            RectangleF vb = vr.GetBounds();
            float sx = x0 + w.Length * cell + 34, sw = vb.Width + 30, sh = vb.Height + 26;
            float scx = sx + sw / 2, scy = baseY - 30;
            using (Matrix m = new Matrix())
            {
                m.Translate(scx, scy); m.Rotate(-5f); m.Translate(-(vb.X + vb.Width / 2), -(vb.Y + vb.Height / 2));
                vr.Transform(m);
            }
            GraphicsPath box = new GraphicsPath();
            box.AddRectangle(new RectangleF(-sw / 2, -sh / 2, sw, sh));
            using (Matrix m = new Matrix()) { m.Translate(scx, scy); m.Rotate(-5f); box.Transform(m); }
            stamp.Add(vr);
            // the release, struck under it at a slightly different angle
            GraphicsPath rel = new GraphicsPath();
            rel.AddString(Program.Release, fam, (int)FontStyle.Bold, 26f, PointF.Empty, StringFormat.GenericTypographic);
            RectangleF rb = rel.GetBounds();
            float rw = rb.Width + 22, rh = rb.Height + 16, rcx = scx + 6, rcy = baseY + 40;
            using (Matrix m = new Matrix())
            {
                m.Translate(rcx, rcy); m.Rotate(-2.5f); m.Translate(-(rb.X + rb.Width / 2), -(rb.Y + rb.Height / 2));
                rel.Transform(m);
            }
            GraphicsPath relBox = new GraphicsPath();
            relBox.AddRectangle(new RectangleF(-rw / 2, -rh / 2, rw, rh));
            using (Matrix m = new Matrix()) { m.Translate(rcx, rcy); m.Rotate(-2.5f); relBox.Transform(m); }

            Color ink = Color.FromArgb(255, 222, 244, 222), rust = Color.FromArgb(255, 236, 104, 34);
            title = new Bitmap(TitleW, TitleH, PixelFormat.Format32bppArgb);
            using (Graphics g = Graphics.FromImage(title))
            {
                g.Clear(Color.Transparent); g.SmoothingMode = SmoothingMode.AntiAlias;
                using (SolidBrush b = new SolidBrush(ink))
                using (Pen bleed = new Pen(Color.FromArgb(120, ink), 1.6f))
                    foreach (GraphicsPath p in word) { g.FillPath(b, p); g.DrawPath(bleed, p); }
                using (SolidBrush b = new SolidBrush(rust)) g.FillPath(b, vr);
                using (Pen p = new Pen(rust, 5f)) { p.LineJoin = LineJoin.Miter; g.DrawPath(p, box); }
                using (SolidBrush b = new SolidBrush(rust)) g.FillPath(b, rel);
                using (Pen p = new Pen(rust, 3f)) { p.LineJoin = LineJoin.Miter; g.DrawPath(p, relBox); }
            }
            // distress: worn ink, holes, scratches
            int[] px = U.ReadPixels(title);
            Random d = new Random(31);
            double[] scratchY = new double[7], scratchK = new double[7];
            for (int k = 0; k < 7; k++) { scratchY[k] = 20 + d.NextDouble() * 110; scratchK[k] = (d.NextDouble() - 0.5) * 0.08; }
            for (int y = 0; y < TitleH; y++)
                for (int x = 0; x < TitleW; x++)
                {
                    int i = y * TitleW + x; int a = (px[i] >> 24) & 255; if (a == 0) continue;
                    double n = VNoise(x / 6.0, y / 6.0, 1) * 0.65 + VNoise(x / 2.2, y / 2.2, 2) * 0.35;
                    double keep = U.Smooth(0.14, 0.29, n);
                    if (d.NextDouble() < 0.035) keep *= 0.4;
                    for (int k = 0; k < 7; k++) if (Math.Abs(y - (scratchY[k] + x * scratchK[k])) < 0.8 && VNoise(x / 30.0, k, 3) > 0.45) keep *= 0.12;
                    keep *= 0.82 + 0.18 * VNoise(x / 14.0, y / 14.0, 4);
                    int na = (int)(a * keep);
                    px[i] = (na << 24) | (px[i] & 0xFFFFFF);
                }
            title.Dispose();
            title = U.FromPixels(px, TitleW, TitleH, PixelFormat.Format32bppArgb);
            int[] rp = new int[px.Length], cp = new int[px.Length];
            for (int i = 0; i < px.Length; i++)
            {
                int a = ((px[i] >> 24) & 255) * 60 / 100;
                rp[i] = (a << 24) | (255 << 16) | (40 << 8) | 60;
                cp[i] = (a << 24) | (40 << 16) | (255 << 8) | 240;
            }
            red = U.FromPixels(rp, TitleW, TitleH, PixelFormat.Format32bppArgb);
            cyan = U.FromPixels(cp, TitleW, TitleH, PixelFormat.Format32bppArgb);
            // soft green glow (layered strokes instead of a blur)
            glow = new Bitmap(TitleW + GlowPad * 2, TitleH + GlowPad * 2, PixelFormat.Format32bppPArgb);
            using (Graphics g = Graphics.FromImage(glow))
            {
                g.Clear(Color.Transparent); g.SmoothingMode = SmoothingMode.AntiAlias;
                g.TranslateTransform(GlowPad, GlowPad);
                float[] widths = { 26, 18, 11, 6 };
                foreach (float pw in widths)
                {
                    using (Pen p = new Pen(Color.FromArgb(12, 100, 255, 130), pw))
                    {
                        p.LineJoin = LineJoin.Round;
                        foreach (GraphicsPath gp in word) g.DrawPath(p, gp);
                    }
                    using (Pen p = new Pen(Color.FromArgb(10, 255, 120, 40), pw)) { p.LineJoin = LineJoin.Round; g.DrawPath(p, box); g.DrawPath(p, vr); g.DrawPath(p, relBox); g.DrawPath(p, rel); }
                }
            }
            foreach (GraphicsPath p in word) p.Dispose();
            vr.Dispose(); box.Dispose(); rel.Dispose(); relBox.Dispose();
        }

        public static Bitmap RoundLogo(Image src, int size)
        {
            Bitmap b = new Bitmap(size, size, PixelFormat.Format32bppPArgb);
            using (Graphics g = Graphics.FromImage(b))
            {
                g.Clear(Color.Transparent);
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                using (GraphicsPath p = new GraphicsPath())
                {
                    p.AddEllipse(0, 0, size - 1, size - 1);
                    g.SetClip(p);
                    g.DrawImage(src, new Rectangle(-1, -1, size + 2, size + 2));
                    g.ResetClip();
                }
            }
            return b;
        }
    }

    // ---------------------------------------------------------------------------------
    //  Icon (drawn at runtime, also written to Outlast1VR.ico by --write-icon)
    // ---------------------------------------------------------------------------------
    static class IconMaker
    {
        public static Bitmap Draw(int s)
        {
            Bitmap b = new Bitmap(s, s, PixelFormat.Format32bppArgb);
            using (Graphics g = Graphics.FromImage(b))
            {
                g.Clear(Color.Transparent);
                g.SmoothingMode = SmoothingMode.AntiAlias; g.TextRenderingHint = TextRenderingHint.AntiAlias;
                float k = s / 32f;
                using (GraphicsPath p = new GraphicsPath())
                {
                    float rr = 6 * k, w = s - 1;
                    p.AddArc(0, 0, rr * 2, rr * 2, 180, 90); p.AddArc(w - rr * 2, 0, rr * 2, rr * 2, 270, 90);
                    p.AddArc(w - rr * 2, w - rr * 2, rr * 2, rr * 2, 0, 90); p.AddArc(0, w - rr * 2, rr * 2, rr * 2, 90, 90); p.CloseFigure();
                    using (LinearGradientBrush lb = new LinearGradientBrush(new Rectangle(0, 0, s, s), Color.FromArgb(255, 18, 52, 26), Color.FromArgb(255, 3, 10, 5), 90f)) g.FillPath(lb, p);
                    using (Pen pen = new Pen(Color.FromArgb(255, 120, 230, 140), Math.Max(1f, 1.4f * k))) g.DrawPath(pen, p);
                }
                using (SolidBrush red = new SolidBrush(Color.FromArgb(255, 255, 45, 35))) g.FillEllipse(red, 5 * k, 5 * k, 7 * k, 7 * k);
                if (s >= 24)
                {
                    using (Font f = U.MakeFont(U.Type, 15 * k, FontStyle.Bold))
                    using (SolidBrush t = new SolidBrush(Color.FromArgb(255, 215, 250, 215)))
                    {
                        StringFormat sf = new StringFormat(); sf.Alignment = StringAlignment.Center; sf.LineAlignment = StringAlignment.Center;
                        g.DrawString("VR", f, t, new RectangleF(0, 4 * k, s, s), sf);
                    }
                }
                else
                {
                    using (Pen pen = new Pen(Color.FromArgb(255, 215, 250, 215), 1.5f)) { g.DrawLine(pen, 4, 11, 8, 14); g.DrawLine(pen, 8, 14, 12, 11); }
                }
            }
            return b;
        }

        public static Icon Make()
        {
            try { using (Bitmap b = Draw(32)) return Icon.FromHandle(b.GetHicon()); } catch { return null; }
        }

        public static void WriteIco(string path)
        {
            int[] sizes = { 16, 24, 32, 48, 64, 256 };
            List<byte[]> pngs = new List<byte[]>();
            foreach (int s in sizes) using (Bitmap b = Draw(s)) using (MemoryStream ms = new MemoryStream()) { b.Save(ms, ImageFormat.Png); pngs.Add(ms.ToArray()); }
            using (MemoryStream o = new MemoryStream())
            using (BinaryWriter w = new BinaryWriter(o))
            {
                w.Write((short)0); w.Write((short)1); w.Write((short)sizes.Length);
                int offset = 6 + 16 * sizes.Length;
                for (int i = 0; i < sizes.Length; i++)
                {
                    w.Write((byte)(sizes[i] >= 256 ? 0 : sizes[i])); w.Write((byte)(sizes[i] >= 256 ? 0 : sizes[i]));
                    w.Write((byte)0); w.Write((byte)0); w.Write((short)1); w.Write((short)32);
                    w.Write(pngs[i].Length); w.Write(offset); offset += pngs[i].Length;
                }
                foreach (byte[] p in pngs) w.Write(p);
                w.Flush();
                File.WriteAllBytes(Path.GetFullPath(path), o.ToArray());
            }
        }
    }

    // ---------------------------------------------------------------------------------
    //  The viewfinder: state, rendering and actions (no WinForms dependency)
    // ---------------------------------------------------------------------------------
    sealed class LogLine { public string Text; public Tone Kind; public double At; }

    sealed class Row { public string Tag, Key, Value; public Tone Kind; }

    sealed partial class Viewfinder : IDisposable
    {
        public const int W = 1280, H = 720;
        public const double BootEnd = 2.1;
        public const int HitNone = -1, HitSpeaker = 10, HitMin = 11, HitClose = 12;

        // Test38: VR SETTINGS and QUEST 3 CONTROLS open their own tapes (Outlast1VR_Pages.cs); the log
        // folder moved onto the settings tape.
        public static readonly string[] Labels = { "PLAY IN VR", "VR SETTINGS", "QUEST 3 CONTROLS", "INSTALL SETTINGS ONLY", "UNINSTALL VR MOD", "EXIT" };
        static readonly string[] Subs =
        {
            "Install the VR profile, then start Outlast",
            "Turning, comfort, the hands-on controls, picture",
            "Every button and gesture on the Touch Plus controllers",
            "Apply the VR ini profile without launching",
            "Remove the mod, put the game's own settings back",
            "Stop recording and eject the tape",
        };
        public const int BtnPlay = 0, BtnSettings = 1, BtnControls = 2, BtnInstall = 3, BtnRestore = 4, BtnExit = 5;
        public static readonly Rectangle[] Btn =
        {
            new Rectangle(56, 244, 468, 80), new Rectangle(56, 332, 468, 50), new Rectangle(56, 388, 468, 50),
            new Rectangle(56, 444, 468, 50), new Rectangle(56, 500, 468, 50), new Rectangle(56, 556, 468, 50),
        };
        static readonly Rectangle PanelR = new Rectangle(792, 392, 432, 242);
        static readonly Rectangle SpeakerR = new Rectangle(1000, 36, 34, 28), MinR = new Rectangle(1204, 2, 34, 24), CloseR = new Rectangle(1240, 2, 36, 24);
        const string TapeLabel = "TAPE 01 — MOUNT MASSIVE ASYLUM, LAKE COUNTY, COLORADO";

        static readonly Color Osd = Color.FromArgb(240, 228, 246, 228);
        static readonly Color Hi = Color.FromArgb(255, 205, 255, 210);
        static readonly Color NvMain = Color.FromArgb(240, 140, 232, 150);
        static readonly Color NvDim = Color.FromArgb(200, 90, 160, 100);
        static readonly Color Amber = Color.FromArgb(250, 255, 196, 80);
        static readonly Color ErrC = Color.FromArgb(250, 255, 105, 80);
        static readonly Color RecRed = Color.FromArgb(255, 255, 38, 30);

        public Action RequestClose, RequestMinimize;
        public string LastError;

        readonly bool preview;
        readonly string gameDir;
        readonly Stopwatch sw = new Stopwatch();
        double skipOff, holdOff, lastRaw, previewT, lastStatus;

        // assets
        Bitmap outBuf, bandBuf, tmpBuf, overlay, fogA, fogB, bgLit, bgDark, title, titleGlow, titleRed, titleCyan, logo;
        Bitmap[] noiseTiles, staticTiles;
        volatile bool heavyReady;
        PointF focusBg, eyeL, eyeR;
        Bitmap ghost;
        Region moon;

        // fonts and reusable GDI+ objects
        Font fRec, fTc, fOsdSm, fOsdMid, fLabel, fBtnBig, fBtn, fSub, fIdx, fStat, fStatB, fLog, fHint, fCredit, fCenterBig, fCenter, fBoot;
        readonly SolidBrush sb = new SolidBrush(Color.White);
        readonly Pen pen = new Pen(Color.White, 1f);
        readonly StringFormat sf;

        // state
        Status st;
        Row[] rows = new Row[0];
        readonly List<LogLine> log = new List<LogLine>();
        int sel, frame, pressIdx = -1, hover = HitNone;
        double selTime = -10, pressTime = -10, restoreArmUntil = -10, launchAt = -1, exitAt = -1;
        bool gameStarted, closeRequested, ambientStarted, muted;
        int ghostCycle = -1;
        Sfx sfx;

        public Viewfinder(bool preview, string gameDir, bool forceProcedural)
        {
            this.preview = preview;
            this.gameDir = gameDir;
            SysInfo.GameDirForConfig = gameDir;   // (Test46e) Outlast's own Config folder is one of the places looked at
            sf = (StringFormat)StringFormat.GenericTypographic.Clone();
            sf.FormatFlags |= StringFormatFlags.MeasureTrailingSpaces | StringFormatFlags.NoWrap;

            outBuf = new Bitmap(W, H, PixelFormat.Format32bppPArgb);
            bandBuf = new Bitmap(W, 64, PixelFormat.Format32bppPArgb);
            MakeFonts();
            overlay = Art.Overlay(W, H);
            noiseTiles = new Bitmap[6];
            for (int i = 0; i < noiseTiles.Length; i++) noiseTiles[i] = Art.NoiseTile(100 + i, false, 1);
            staticTiles = new Bitmap[4];
            for (int i = 0; i < staticTiles.Length; i++) staticTiles[i] = Art.NoiseTile(200 + i, true, 2);
            using (GraphicsPath a = new GraphicsPath())
            using (GraphicsPath b = new GraphicsPath())
            {
                a.AddEllipse(1046, 43, 15, 15); b.AddEllipse(1051, 40, 15, 15);
                moon = new Region(a); moon.Exclude(b);
            }

            st = Status.Read(gameDir);
            BuildRows();
            if (!preview) muted = ReadMuted();
            LoadLogo();
            string splash = forceProcedural ? null : SysInfo.FindSplash(gameDir);
            if (preview) BuildHeavy(splash);
            else ThreadPool.QueueUserWorkItem(delegate { BuildHeavy(splash); });
            StartupLog();
        }

        void MakeFonts()
        {
            fRec = U.MakeFont(U.Mono, 23, FontStyle.Bold);
            fTc = U.MakeFont(U.Mono, 21, FontStyle.Bold);
            fOsdSm = U.MakeFont(U.Mono, 13, FontStyle.Bold);
            fOsdMid = U.MakeFont(U.Mono, 15, FontStyle.Bold);
            fLabel = U.MakeFont(U.Mono, 15, FontStyle.Bold);
            fBtnBig = U.MakeFont(U.Mono, 31, FontStyle.Bold);
            fBtn = U.MakeFont(U.Mono, 21, FontStyle.Bold);
            fSub = U.MakeFont(U.Mono, 12.5f, FontStyle.Regular);
            fIdx = U.MakeFont(U.Mono, 12, FontStyle.Bold);
            fStat = U.MakeFont(U.Mono, 13, FontStyle.Regular);
            fStatB = U.MakeFont(U.Mono, 13, FontStyle.Bold);
            fLog = U.MakeFont(U.Mono, 12.5f, FontStyle.Regular);
            fHint = U.MakeFont(U.Mono, 12, FontStyle.Bold);
            fCredit = U.MakeFont(U.Mono, 17, FontStyle.Bold);
            fCenterBig = U.MakeFont(U.Type, 36, FontStyle.Bold);
            fCenter = U.MakeFont(U.Mono, 18, FontStyle.Bold);
            fBoot = U.MakeFont(U.Mono, 20, FontStyle.Bold);
        }

        void BuildHeavy(string splash)
        {
            try
            {
                Bitmap t, gl, rd, cy;
                Art.Title(out t, out gl, out rd, out cy);
                titleGlow = gl; titleRed = rd; titleCyan = cy; title = t;
                fogA = Art.Fog(5, 420, 30); fogB = Art.Fog(9, 360, 22);
                Bitmap a, b;
                Art.Corridor(out a, out b);
                bgLit = a; bgDark = b; focusBg = Art.FocusBg(); eyeL = Art.EyeBg(-1); eyeR = Art.EyeBg(1);
                if (splash != null) { try { ghost = Art.Ghost(splash); } catch { ghost = null; } }
            }
            catch (Exception ex)
            {
                LastError = ex.Message;
                if (bgLit == null) { bgLit = new Bitmap(Art.BW, Art.BH, PixelFormat.Format32bppPArgb); using (Graphics g = Graphics.FromImage(bgLit)) g.Clear(Color.FromArgb(4, 14, 7)); bgDark = bgLit; }
            }
            heavyReady = true;
        }

        void LoadLogo()
        {
            Image src = null;
            try
            {
                string p = Path.Combine(Program.AppDir(), "strikezone_logo.jpg");
                if (File.Exists(p)) using (Image i = Image.FromFile(p)) src = new Bitmap(i);
            }
            catch { src = null; }
            if (src == null)
            {
                try
                {
                    using (Stream s = Assembly.GetExecutingAssembly().GetManifestResourceStream("Outlast1VR.strikezone_logo.jpg"))
                        if (s != null) using (Image i = Image.FromStream(s)) src = new Bitmap(i);
                }
                catch { src = null; }
            }
            if (src != null) { logo = Art.RoundLogo(src, 60); src.Dispose(); }
        }

        // ---------------- clock ----------------

        public void StartClock() { sw.Reset(); sw.Start(); lastRaw = 0; }
        public double T { get { return preview ? previewT : sw.Elapsed.TotalSeconds + skipOff - holdOff; } }
        public bool Booting { get { return T < BootEnd; } }
        bool Busy { get { return launchAt >= 0 || exitAt >= 0; } }

        public void SkipBoot()
        {
            double t = T;
            if (t >= BootEnd) return;
            skipOff += BootEnd - t;
            if (sfx != null) { sfx.StopBoot(); if (sfx.StartAmbient()) ambientStarted = true; }
        }

        public void MuteForSession() { muted = true; if (sfx != null) sfx.Muted = true; }   // --perf-log: silent, not saved

        public void StartAudio()
        {
            if (preview) return;
            try { sfx = new Sfx(muted); sfx.Boot(); } catch { sfx = null; }
        }

        public void Tick()
        {
            double raw = sw.Elapsed.TotalSeconds, dt = raw - lastRaw; lastRaw = raw;
            if (!heavyReady && T > 0.9) holdOff += dt;
            double t = T;
            if (sfx != null && !ambientStarted && t > 1.3 && !Busy && sfx.StartAmbient()) ambientStarted = true;
            if (sfx != null && ghost != null && t > 6 && !Busy)
            {
                int cyc = (int)(t / 41.0);
                if (t % 41.0 >= 23.95 && cyc != ghostCycle) { ghostCycle = cyc; sfx.Ghost(); }
            }
            if (launchAt >= 0)
            {
                double lt = t - launchAt;
                if (!gameStarted && lt >= 0.95) StartGame();
                if (launchAt >= 0 && lt >= 2.55) Close();
            }
            if (exitAt >= 0 && t - exitAt >= 0.5) Close();
            if (t - lastStatus > 4 && !Busy && t > BootEnd) RefreshStatus();
        }

        void Close()
        {
            if (closeRequested) return;
            closeRequested = true;
            if (RequestClose != null) RequestClose();
        }

        // ---------------- status + log ----------------

        void RefreshStatus()
        {
            lastStatus = T;
            st = Status.Read(gameDir);
            BuildRows();
        }

        static string ShortPath(string p, int max)
        {
            if (string.IsNullOrEmpty(p)) return "";
            if (p.Length <= max) return p;
            string[] parts = p.Split('\\');
            string s = parts[parts.Length - 1];
            for (int i = parts.Length - 2; i >= 0; i--)
            {
                string c = parts[i] + "\\" + s;
                if (c.Length + 4 > max) break;
                s = c;
            }
            s = "...\\" + s;
            return s.Length > max ? "..." + s.Substring(s.Length - max + 3) : s;
        }

        void BuildRows()
        {
            List<Row> r = new List<Row>();
            if (st.GameFound) r.Add(MakeRow("OK", "GAME", ShortPath(st.GameDir, 38), Tone.Ok));
            else r.Add(MakeRow("!!", "GAME", "OLGame.exe NOT NEXT TO LAUNCHER", Tone.Err));
            if (!st.GameFound) r.Add(MakeRow("--", "VR MOD", "WAITING FOR GAME FOLDER", Tone.Info));
            else if (st.D3d9 && st.Loader) r.Add(MakeRow("OK", "VR MOD", "d3d9.dll + openxr_loader.dll", Tone.Ok));
            else if (st.D3d9) r.Add(MakeRow("!!", "VR MOD", "openxr_loader.dll MISSING", Tone.Warn));
            else r.Add(MakeRow("!!", "VR MOD", "d3d9.dll MISSING", Tone.Err));
            if (st.Build != null) r.Add(MakeRow("OK", "BUILD", st.Build.Length > 38 ? st.Build.Substring(0, 38) : st.Build, Tone.Ok));
            else r.Add(MakeRow("--", "BUILD", st.D3d9 ? "UNKNOWN (PLAY ONCE)" : Program.Release, Tone.Info));
            if (st.RuntimeName != null) r.Add(MakeRow("OK", "OPENXR", st.RuntimeName, Tone.Ok));
            else r.Add(MakeRow("!!", "OPENXR", "NO ACTIVE RUNTIME SET", Tone.Warn));
            if (!st.ConfigFound) r.Add(MakeRow("!!", "PROFILE", "NO CONFIG - RUN OUTLAST ONCE", Tone.Warn));
            else if (st.Installed) r.Add(MakeRow("OK", "PROFILE", "VR SETTINGS INSTALLED", Tone.Ok));
            else r.Add(MakeRow("--", "PROFILE", "NOT INSTALLED YET", Tone.Info));
            rows = r.ToArray();
        }

        static Row MakeRow(string tag, string key, string val, Tone k) { Row r = new Row(); r.Tag = tag; r.Key = key; r.Value = val; r.Kind = k; return r; }

        void AddLog(string text, Tone kind, double at)
        {
            if (!preview) SysInfo.FileLog(kind + ": " + text);
            LogLine l = new LogLine(); l.Text = text; l.Kind = kind; l.At = at;
            log.Add(l);
            while (log.Count > 24) log.RemoveAt(0);
        }

        void AddLogs(List<Msg> msgs)
        {
            double t = Math.Max(T, BootEnd);
            for (int i = 0; i < msgs.Count; i++) AddLog(msgs[i].Text, msgs[i].Kind, t + i * 0.16);
        }

        void StartupLog()
        {
            if (!st.GameFound)
            {
                AddLog("OLGame.exe NOT FOUND NEXT TO THIS LAUNCHER", Tone.Err, 2.15);
                AddLog("PUT Outlast1VR.exe IN Outlast\\Binaries\\Win64", Tone.Warn, 2.45);
                AddLog("WITH THE OTHER MOD FILES, THEN RUN IT AGAIN", Tone.Warn, 2.75);
            }
            else if (!st.D3d9) AddLog("d3d9.dll MISSING - COPY ALL THE MOD FILES", Tone.Warn, 2.15);
            else if (!st.ConfigFound) AddLog("RUN OUTLAST ONCE (NO CONFIG YET)", Tone.Warn, 2.15);
            else
            {
                AddLog("TAPE LOADED. SYSTEM CHECK COMPLETE.", Tone.Ok, 2.15);
                AddLog("HEADSET ON, THEN PRESS PLAY IN VR.", Tone.Info, 2.45);
            }
        }

        // ---------------- input ----------------

        public int HitTest(Point p)
        {
            if (CloseR.Contains(p)) return HitClose;
            if (MinR.Contains(p)) return HitMin;
            if (SpeakerR.Contains(p)) return HitSpeaker;
            if (page != PageMain) return T > 1.9 ? PageHitTest(p) : HitNone;
            if (T > 1.9) for (int i = 0; i < Btn.Length; i++) if (Btn[i].Contains(p)) return i;
            return HitNone;
        }

        public bool MouseMove(Point p)
        {
            int h = HitTest(p);
            hover = h;
            if (page != PageMain) return h != HitNone;
            if (h >= 0 && h < Btn.Length && !Busy) Select(h);
            return h != HitNone;
        }

        public void Select(int i)
        {
            if (i == sel) return;
            sel = i; selTime = T;
            if (sfx != null) sfx.Tick();
        }

        public void Move(int d)
        {
            if (Busy) return;
            if (page == PageSettings) { SettingsSelect(setSel + d); return; }
            if (page == PageControls) { PageKey(d < 0 ? Keys.Up : Keys.Down, false); return; }
            int n = Btn.Length;
            Select(((sel + d) % n + n) % n);
        }

        public void Click(int hit)
        {
            if (hit == HitClose) { BeginExit(); return; }
            if (hit == HitMin) { if (RequestMinimize != null) RequestMinimize(); return; }
            if (hit == HitSpeaker) { ToggleMute(); return; }
            if (page != PageMain) { PageClick(hit); return; }
            if (hit >= 0 && hit < Btn.Length) { Select(hit); Activate(hit); }
        }

        public void ToggleMute()
        {
            muted = !muted;
            if (sfx != null) sfx.Muted = muted;
            if (!preview) WriteMuted(muted);
        }

        static bool ReadMuted()
        {
            try { using (RegistryKey k = Registry.CurrentUser.OpenSubKey(@"Software\OutlastVR\Launcher")) return k != null && Convert.ToInt32(k.GetValue("Muted", 0)) != 0; }
            catch { return false; }
        }

        static void WriteMuted(bool m)
        {
            try { using (RegistryKey k = Registry.CurrentUser.CreateSubKey(@"Software\OutlastVR\Launcher")) k.SetValue("Muted", m ? 1 : 0, RegistryValueKind.DWord); }
            catch { }
        }

        public void Activate(int i)
        {
            if (Busy || T < 1.9) return;
            pressIdx = i; pressTime = T;
            switch (i)
            {
                case BtnPlay: DoInstall(true); break;
                case BtnSettings: OpenPage(PageSettings); break;
                case BtnControls: OpenPage(PageControls); break;
                case BtnInstall: DoInstall(false); break;
                case BtnRestore: DoRestore(); break;
                default: BeginExit(); break;
            }
        }

        bool Precheck(bool needMod, bool needConfig, List<Msg> m)
        {
            RefreshStatus();
            if (!st.GameFound)
            {
                m.Add(new Msg("OLGame.exe NOT FOUND NEXT TO THIS LAUNCHER", Tone.Err));
                m.Add(new Msg("PUT IT IN Outlast\\Binaries\\Win64 AND RETRY", Tone.Warn));
                return false;
            }
            if (needMod && !st.D3d9) { m.Add(new Msg("d3d9.dll IS MISSING - COPY ALL THE MOD FILES", Tone.Err)); return false; }
            if (SysInfo.GameRunning()) { m.Add(new Msg("CLOSE OUTLAST FIRST", Tone.Err)); return false; }
            if (needConfig && !st.ConfigFound)
            {
                m.Add(new Msg("OUTLAST'S SETTINGS NOT FOUND", Tone.Err));
                m.Add(new Msg("START OUTLAST FROM STEAM, REACH THE MENU, QUIT, RETRY", Tone.Warn));
                return false;
            }
            return true;
        }

        void DoInstall(bool launch)
        {
            if (sfx != null) sfx.Confirm();
            List<Msg> m = new List<Msg>();
            bool ok = Precheck(true, true, m);
            if (ok)
            {
                m.Add(new Msg("SETTINGS: " + ShortPath(st.ConfigDir, 44), Tone.Info));   // (Test46e) which folder
                Size res = SysInfo.PhysicalPrimary();
                ok = Installer.Install(st.ConfigDir, res.Width, res.Height, m, preview);
            }
            if (ok && launch) m.Add(new Msg("STARTING OUTLAST VR...", Tone.Ok));
            AddLogs(m);
            RefreshStatus();
            if (!ok) { if (sfx != null) sfx.Error(); return; }
            if (launch)
            {
                launchAt = T;
                if (sfx != null) { sfx.StopAmbient(1.2); sfx.Heartbeat(); }
            }
            else if (sfx != null) sfx.Success();
        }

        void DoRestore()
        {
            double t = T;
            if (restoreArmUntil < t)
            {
                restoreArmUntil = t + 4;
                AddLog("PRESS UNINSTALL AGAIN TO CONFIRM", Tone.Warn, t);
                if (sfx != null) sfx.Armed();
                return;
            }
            restoreArmUntil = -10;
            if (sfx != null) sfx.Confirm();
            List<Msg> m = new List<Msg>();
            bool ok = Precheck(false, false, m) && Installer.Restore(SysInfo.ConfigDir(), gameDir, m, preview);
            if (ok && !preview && string.Equals(Path.GetFullPath(Program.AppDir()), Path.GetFullPath(gameDir), StringComparison.OrdinalIgnoreCase))
            {
                Program.SelfDeleteOnExit = true;
                m.Add(new Msg("THIS LAUNCHER DELETES ITSELF WHEN YOU CLOSE IT", Tone.Info));
            }
            AddLogs(m);
            RefreshStatus();
            if (sfx != null) { if (ok) sfx.Success(); else sfx.Error(); }
        }

        void DoOpenLogs()
        {
            if (sfx != null) sfx.Confirm();
            string dir = SysInfo.LogDir();
            if (!Directory.Exists(dir)) { AddLog("NO LOG FOLDER YET - PLAY IN VR ONCE", Tone.Warn, T); if (sfx != null) sfx.Error(); return; }
            if (preview) { AddLog("OPENED " + ShortPath(dir, 44), Tone.Ok, T); return; }
            try
            {
                ProcessStartInfo psi = new ProcessStartInfo("explorer.exe", "\"" + dir + "\"");
                psi.UseShellExecute = true;
                Process.Start(psi);
                AddLog("OPENED %LOCALAPPDATA%\\OutlastVR", Tone.Ok, T);
            }
            catch (Exception ex) { AddLog("COULD NOT OPEN FOLDER: " + ex.Message, Tone.Err, T); }
        }

        void StartGame()
        {
            gameStarted = true;
            if (preview) return;
            try
            {
                ProcessStartInfo psi = new ProcessStartInfo(Path.Combine(gameDir, "OLGame.exe"), "-installed -seekfreeloadingpcconsole");
                psi.WorkingDirectory = gameDir;
                psi.UseShellExecute = true;
                Process.Start(psi);
            }
            catch (Exception ex)
            {
                launchAt = -1; gameStarted = false;
                AddLog("COULD NOT START OLGame.exe: " + (ex.Message.Length > 30 ? ex.Message.Substring(0, 30) : ex.Message), Tone.Err, T);
                if (sfx != null) { sfx.Error(); ambientStarted = false; }
            }
        }

        public void BeginExit()
        {
            if (exitAt >= 0) { Close(); return; }
            if (launchAt >= 0) { if (gameStarted) Close(); return; }
            exitAt = T;
            if (sfx != null) { sfx.StopBoot(); sfx.StopAmbient(0.3); sfx.PowerOff(); }
        }

        public void HandleKey(Keys k, bool shift)
        {
            if (page != PageMain && !Booting && !Busy)
            {
                if (k == Keys.M) { ToggleMute(); return; }
                PageKey(k, shift);
                return;
            }
            if (k == Keys.Escape) { BeginExit(); return; }
            if (Booting) { SkipBoot(); return; }
            switch (k)
            {
                case Keys.Up: case Keys.W: Move(-1); break;
                case Keys.Down: case Keys.S: Move(1); break;
                case Keys.Tab: Move(shift ? -1 : 1); break;
                case Keys.Home: Select(0); break;
                case Keys.End: Select(Btn.Length - 1); break;
                case Keys.Enter: case Keys.Space: Activate(sel); break;
                case Keys.M: ToggleMute(); break;
            }
        }

        public void PadBack() { if (Busy) return; if (page != PageMain) ClosePage(); else Select(Btn.Length - 1); }
        public List<string> LogTexts() { List<string> l = new List<string>(); foreach (LogLine x in log) l.Add(x.Kind + ": " + x.Text); return l; }
        public int Hover { get { return hover; } }

        // ---------------- preview / bench hooks ----------------

        public void SetPreview(double t, int hoverIdx, double hoverAge)
        {
            previewT = t; sel = Math.Max(0, Math.Min(Btn.Length - 1, hoverIdx)); selTime = t - hoverAge;
        }

        public void PreviewArmRestore(double t) { restoreArmUntil = t + 3; AddLog("PRESS UNINSTALL AGAIN TO CONFIRM", Tone.Warn, t - 1); }
        public void PreviewLaunch(double t, double since) { launchAt = t - since; gameStarted = since >= 0.95; }
        public void PreviewExit(double t, double since) { exitAt = t - since; }

        public void PreviewDryInstall(double at)
        {
            List<Msg> m = new List<Msg>();
            Size res = SysInfo.PhysicalPrimary();
            if (Installer.Install(SysInfo.ConfigDir(), res.Width, res.Height, m, true)) { }
            for (int i = 0; i < m.Count; i++) AddLog(m[i].Text, m[i].Kind, at + i * 0.16);
        }

        // ---------------- rendering ----------------

        public Bitmap Render()
        {
            using (Graphics g = Graphics.FromImage(outBuf))
            {
                try { RenderFrame(g, T); }
                catch (Exception ex) { LastError = ex.ToString(); }
            }
            return outBuf;
        }

        void Fill(Graphics g, Color c, int x, int y, int w, int h) { sb.Color = c; g.FillRectangle(sb, x, y, w, h); }
        void Fill(Graphics g, Color c, Rectangle r) { sb.Color = c; g.FillRectangle(sb, r); }

        void Text(Graphics g, string s, Font f, Color c, float x, float y, bool shadow)
        {
            if (shadow) { sb.Color = Color.FromArgb(c.A * 150 / 255, 0, 0, 0); g.DrawString(s, f, sb, x + 1.5f, y + 1.5f, sf); }
            sb.Color = c; g.DrawString(s, f, sb, x, y, sf);
        }

        float Measure(Graphics g, string s, Font f) { return g.MeasureString(s, f, PointF.Empty, sf).Width; }

        void TextRight(Graphics g, string s, Font f, Color c, float right, float y, bool shadow) { Text(g, s, f, c, right - Measure(g, s, f), y, shadow); }
        void TextCenter(Graphics g, string s, Font f, Color c, float cx, float y, bool shadow) { Text(g, s, f, c, cx - Measure(g, s, f) / 2, y, shadow); }

        static void DrawImageA(Graphics g, Image img, int x, int y, double alpha)
        {
            if (img == null || alpha <= 0.004) return;
            if (alpha >= 0.996) { g.DrawImage(img, x, y, img.Width, img.Height); return; }
            using (ImageAttributes ia = new ImageAttributes())
            {
                ColorMatrix cm = new ColorMatrix(); cm.Matrix33 = (float)alpha;
                ia.SetColorMatrix(cm);
                g.DrawImage(img, new Rectangle(x, y, img.Width, img.Height), 0, 0, img.Width, img.Height, GraphicsUnit.Pixel, ia);
            }
        }

        static void DrawImagePart(Graphics g, Image img, Rectangle dst, Rectangle src, double alpha)
        {
            if (alpha <= 0.004) return;
            if (alpha >= 0.996) { g.DrawImage(img, dst, src, GraphicsUnit.Pixel); return; }
            using (ImageAttributes ia = new ImageAttributes())
            {
                ColorMatrix cm = new ColorMatrix(); cm.Matrix33 = (float)alpha;
                ia.SetColorMatrix(cm);
                g.DrawImage(img, dst, src.X, src.Y, src.Width, src.Height, GraphicsUnit.Pixel, ia);
            }
        }

        void RenderFrame(Graphics g, double t)
        {
            frame = (int)Math.Floor(t * 30.0);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.AntiAlias;
            g.InterpolationMode = InterpolationMode.NearestNeighbor;
            g.CompositingQuality = CompositingQuality.HighSpeed;

            if (t < 0.30) { DrawBlack(g, t); DrawWindowButtons(g); return; }
            if (t < 0.95 || !heavyReady) { DrawStatic(g, t); DrawWindowButtons(g); return; }

            Mark(-1);
            DrawScene(g, t); Mark(0);
            double ua = UiAlpha(t);
            if (ua > 0.004) { if (page != PageMain) DrawPage(g, t, ua); else DrawUi(g, t, ua); }
            Mark(1);
            DrawTrackingBand(g, t); Mark(2);
            DrawNoise(g, t); Mark(3);
            g.DrawImage(overlay, 0, 0, W, H); Mark(4);
            DrawRollBar(g, t);
            DrawHeadSwitch(g, t); Mark(5);
            DrawOsd(g, t); Mark(6);
            if (t - pageAt < 0.32) DrawTapeChange(g, t);
            if (t < 1.65) DrawPowerOn(g, t);
            if (launchAt >= 0) DrawLaunch(g, t - launchAt);
            double c = 0;
            if (launchAt >= 0) c = U.Smooth(1.98, 2.48, t - launchAt);
            if (exitAt >= 0) c = Math.Max(c, U.Smooth(0, 0.44, t - exitAt));
            if (c > 0) DrawCollapse(g, c);
            DrawWindowButtons(g);
        }

        public static double[] Prof = new double[8];
        static readonly Stopwatch profSw = Stopwatch.StartNew();
        static double profLast;
        static void Mark(int stage)
        {
            double now = profSw.Elapsed.TotalMilliseconds;
            if (stage >= 0) Prof[stage] += now - profLast;
            profLast = now;
        }

        double UiAlpha(double t)
        {
            double a = U.Smooth(1.55, 2.1, t);
            if (launchAt >= 0) a *= 1 - U.Smooth(0.05, 0.6, t - launchAt);
            if (exitAt >= 0) a *= 1 - U.Smooth(0, 0.2, t - exitAt);
            return a;
        }

        // Brief double exposure of the user's own splash image, about every 41 s (only if their install has one).
        double GhostAmount(double t)
        {
            if (ghost == null || t < 6 || launchAt >= 0 || exitAt >= 0) return 0;
            double c = t % 41.0;
            if (c < 24.0 || c > 25.1) return 0;
            double env = U.Smooth(24.0, 24.25, c) * (1 - U.Smooth(24.75, 25.1, c));
            double flick = U.Hash((int)(t * 15), 61) > 0.25 ? 1 : 0.35;
            return env * flick * 0.8;
        }

        bool LampOn(double t)
        {
            double cyc = t % 7.3; int n = (int)(t / 7.3);
            if (cyc > 5.7 && cyc < 6.5) return U.Hash((int)(t * 14), 11) > 0.42;
            if (U.Hash(n, 5) > 0.6 && cyc > 2.0 && cyc < 2.12) return false;
            return true;
        }

        void DrawBlack(Graphics g, double t)
        {
            g.Clear(Color.Black);
            if (t > 0.04 && t < 0.09) Fill(g, Color.FromArgb(18, 120, 160, 130), 0, 0, W, H);
            if (((int)(t * 8)) % 2 == 0) Text(g, "LOADING TAPE", fOsdMid, Color.FromArgb(150, 200, 230, 200), 60, 656, false);
        }

        void DrawStatic(Graphics g, double t)
        {
            g.CompositingMode = CompositingMode.SourceCopy;
            Tiles(g, staticTiles, 3);
            g.CompositingMode = CompositingMode.SourceOver;
            double env = U.Smooth(0.30, 0.42, t) * (1 - U.Smooth(0.86, 0.95, t));
            Fill(g, Color.FromArgb((int)(255 - 175 * env), 0, 0, 0), 0, 0, W, H);
            for (int k = 0; k < 7; k++)
            {
                int y = (int)(U.Hash(k, frame * 7) * H), hh = 3 + (int)(U.Hash(k + 20, frame) * 26);
                bool white = U.Hash(k + 40, frame) > 0.5;
                Fill(g, white ? Color.FromArgb((int)(70 * env), 220, 255, 225) : Color.FromArgb((int)(150 * env), 0, 0, 0), 0, y, W, hh);
            }
            int roll = (int)(((t - 0.3) * 1100) % (H + 120)) - 60;
            Fill(g, Color.FromArgb(170, 0, 0, 0), 0, roll, W, 46);
            // camcorder trying to read the tape
            sb.Color = Color.FromArgb(230, 235, 250, 235);
            g.FillPolygon(sb, new[] { new PointF(60, 44), new PointF(60, 62), new PointF(75, 53) });
            Text(g, "PLAY", fRec, Osd, 84, 40, true);
            int bars = (int)U.Clamp01((t - 0.3) / 0.6 * 1.0) * 0 + (int)(U.Clamp01((t - 0.3) / 0.6) * 10);
            string bar = new string('█', bars) + new string('░', 10 - bars);
            TextCenter(g, "TRACKING", fBoot, Osd, W / 2f, H / 2f - 26, true);
            TextCenter(g, bar, fBoot, Osd, W / 2f, H / 2f + 4, true);
            if (((int)(t * 6)) % 2 == 0) Text(g, "TAPE 01", fOsdMid, Osd, 60, 656, true);
        }

        void DrawScene(Graphics g, double t)
        {
            int sx = (int)Math.Round(Math.Sin(t * 0.61) * 5 + Math.Sin(t * 1.73 + 1.3) * 2);
            int sy = (int)Math.Round(Math.Sin(t * 0.47 + 2.1) * 4 + Math.Sin(t * 1.29) * 1.5);
            double ga = GhostAmount(t);
            bool on = LampOn(t) && ga < 0.05;
            g.CompositingMode = CompositingMode.SourceCopy;
            g.DrawImage(on ? bgLit : bgDark, -20 + sx, -20 + sy, Art.BW, Art.BH);
            g.CompositingMode = CompositingMode.SourceOver;

            DrawEyes(g, t, sx, sy, on);
            if (ga > 0.01) DrawImageA(g, ghost, -20 + sx + (int)((U.Hash(frame, 62) - 0.5) * 10), -20 + sy, ga);
            DrawFog(g, fogA, t * 9, 300 + sy);
            DrawFog(g, fogB, -t * 15, 210 + sy);
            DrawDust(g, t);
            int dark = 6 + (int)(U.Hash(frame, 3) * 12) + (on ? 0 : 26);
            Fill(g, Color.FromArgb(dark, 0, 0, 0), 0, 0, W, H);
            if (U.Hash(frame / 2, 77) > 0.985) Fill(g, Color.FromArgb(14, 200, 255, 205), 0, 0, W, H);
        }

        void DrawEyes(Graphics g, double t, int sx, int sy, bool lampOn)
        {
            double c = t % 19.0;
            double a = U.Smooth(8.0, 8.7, c) * (1 - U.Smooth(13.3, 14.0, c));
            if (c > 11.0 && c < 11.14) a = 0;
            if (a <= 0.01) return;
            a *= lampOn ? 0.75 : 1.0;
            float mx = (eyeL.X + eyeR.X) / 2 - 20 + sx, my = (eyeL.Y + eyeR.Y) / 2 - 20 + sy;
            for (int k = 0; k < 2; k++)
            {
                float x = mx + (k == 0 ? -2.6f : 2.6f), y = my;
                sb.Color = Color.FromArgb((int)(55 * a), 170, 255, 185); g.FillEllipse(sb, x - 5, y - 3.5f, 10, 7);
                sb.Color = Color.FromArgb((int)(235 * a), 235, 255, 235); g.FillEllipse(sb, x - 1.6f, y - 1f, 3.2f, 2.2f);
            }
        }

        void DrawFog(Graphics g, Bitmap fog, double offset, int y)
        {
            if (fog == null) return;
            int x0 = (int)(((offset % W) + W) % W);
            g.DrawImage(fog, x0 - W, y, W, fog.Height);
            g.DrawImage(fog, x0, y, W, fog.Height);
        }

        void DrawDust(Graphics g, double t)
        {
            for (int i = 0; i < 64; i++)
            {
                double hx = U.Hash(i, 1), hy = U.Hash(i, 2), sp = U.Hash(i, 3);
                double x = ((hx * (W + 60) + t * (3 + sp * 11)) % (W + 60)) - 30;
                double y = hy * H + Math.Sin(t * (0.25 + sp * 0.5) + i) * 18 - ((t * (2 + sp * 4)) % 40);
                double tw = 0.5 + 0.5 * Math.Sin(t * (0.8 + sp * 2.5) + i * 1.7);
                int a = (int)(25 + 85 * tw * tw);
                int s = sp > 0.8 ? 2 : 1;
                Fill(g, Color.FromArgb(a, 200, 255, 210), (int)x, (int)y, s, s);
            }
        }

        // Tiles are blitted at integer offsets (a TextureBrush with a transform is ~20x slower in GDI+).
        void Tiles(Graphics g, Bitmap[] tiles, int salt)
        {
            int S = tiles[0].Width;
            int ox = (int)(U.Hash(frame, 5 + salt) * S), oy = (int)(U.Hash(frame, 6 + salt) * S);
            int cell = 0;
            for (int y = -oy; y < H; y += S)
                for (int x = -ox; x < W; x += S, cell++)
                    g.DrawImage(tiles[(int)(U.Hash(cell + salt * 31, frame) * tiles.Length)], x, y, S, S);
        }

        void DrawNoise(Graphics g, double t)
        {
            Tiles(g, noiseTiles, 0);
            if ((launchAt >= 0 && t - launchAt > 1.2) || GhostAmount(t) > 0.05) Tiles(g, noiseTiles, 7);
        }

        void DrawRollBar(Graphics g, double t)
        {
            int y = (int)((t * 85) % (H + 260)) - 130;
            Rectangle r = new Rectangle(0, y, W, 130);
            using (LinearGradientBrush lb = new LinearGradientBrush(new Rectangle(0, y - 1, W, 132), Color.Transparent, Color.Transparent, 90f))
            {
                ColorBlend cb = new ColorBlend(3);
                cb.Colors = new[] { Color.FromArgb(0, 200, 255, 210), Color.FromArgb(16, 200, 255, 210), Color.FromArgb(0, 200, 255, 210) };
                cb.Positions = new[] { 0f, 0.5f, 1f };
                lb.InterpolationColors = cb;
                g.FillRectangle(lb, r);
            }
        }

        void CopyStrip(Graphics g, int y, int h)
        {
            g.Flush(FlushIntention.Sync);
            using (Graphics bg = Graphics.FromImage(bandBuf))
            {
                bg.CompositingMode = CompositingMode.SourceCopy;
                bg.DrawImage(outBuf, new Rectangle(0, 0, W, h), new Rectangle(0, y, W, h), GraphicsUnit.Pixel);
            }
        }

        void DrawTrackingBand(Graphics g, double t)
        {
            double period = 8.7, c = t % period, strength;
            double p;
            if (launchAt >= 0 && t - launchAt > 0.6) { p = ((t - launchAt) * 0.9) % 1.0; strength = 1.0; }
            else
            {
                if (c < 4.0 || c > 5.4) return;
                p = (c - 4.0) / 1.4; strength = Math.Sin(p * Math.PI);
            }
            int bh = 44;
            int y0 = (int)(H + 40 - p * (H + 100)) - bh / 2;
            Rectangle src = Rectangle.Intersect(new Rectangle(0, y0, W, bh), new Rectangle(0, 0, W, H));
            if (src.Height <= 2) return;
            CopyStrip(g, src.Y, src.Height);
            for (int s = 0; s < src.Height; s += 3)
            {
                int sh = Math.Min(3, src.Height - s);
                int dx = (int)(((U.Hash(s + y0 * 7, frame) - 0.5) * 12 + 5) * strength);
                g.DrawImage(bandBuf, new Rectangle(dx, src.Y + s, W, sh), new Rectangle(0, s, W, sh), GraphicsUnit.Pixel);
            }
            Fill(g, Color.FromArgb((int)(16 * strength), 200, 255, 210), src);
            for (int k = 0; k < 26; k++)
            {
                int x = (int)(U.Hash(k, frame * 3) * W), yy = src.Y + (int)(U.Hash(k + 99, frame) * src.Height), len = 8 + (int)(U.Hash(k + 7, frame) * 60);
                Fill(g, Color.FromArgb((int)(90 * strength), 225, 255, 228), x, yy, len, 1);
            }
        }

        void DrawHeadSwitch(Graphics g, double t)
        {
            int h = 8, y = H - h;
            CopyStrip(g, y, h);
            for (int s = 0; s < h; s += 2)
            {
                int dx = 6 + (int)(U.Hash(s, frame) * 12) + s * 2;
                g.DrawImage(bandBuf, new Rectangle(dx, y + s, W, 2), new Rectangle(0, s, W, 2), GraphicsUnit.Pixel);
            }
            for (int k = 0; k < 14; k++)
                Fill(g, Color.FromArgb(110, 220, 255, 225), (int)(U.Hash(k, frame) * W), y + (int)(U.Hash(k + 3, frame) * h), 6 + (int)(U.Hash(k + 5, frame) * 40), 1);
        }

        // ---------------- UI ----------------

        void DrawUi(Graphics g, double t, double ua)
        {
            DrawTitle(g, t, ua);
            // typed tape label
            double ts = Math.Max(0, t - 2.0);
            int n = Math.Min(TapeLabel.Length, (int)(ts * 34));
            bool cursor = n < TapeLabel.Length || ((int)(t * 2.2)) % 2 == 0;
            Text(g, TapeLabel.Substring(0, n) + (cursor ? "█" : ""), fLabel, U.A(NvMain, ua), 60, 198, true);
            Fill(g, U.A(Color.FromArgb(70, 120, 220, 130), ua * U.Smooth(1.9, 2.3, t)), 60, 226, 464, 1);
            for (int i = 0; i < Btn.Length; i++) DrawButton(g, i, t, ua);
            DrawPanel(g, t, ua);
            DrawCredit(g, t, ua);
            double ha = ua * U.Smooth(2.1, 2.6, t);
            TextCenter(g, "↑↓ SELECT    ENTER CONFIRM    M MUTE    F11 FULL SCREEN    ESC EXIT", fHint, U.A(Color.FromArgb(150, 160, 215, 168), ha), 640, 690, true);
        }

        void DrawTitle(Graphics g, double t, double ua)
        {
            if (title == null) return;
            int tx = 50, ty = 66;
            double gl = 0;
            if (t < 2.1) gl = 1 - U.Smooth(1.55, 2.1, t);
            double c = t % 6.3;
            if (c > 4.0 && c < 4.18) gl = Math.Max(gl, 0.7);
            if (U.Hash(frame, 41) > 0.993) gl = Math.Max(gl, 0.4);
            if (launchAt >= 0) gl = Math.Max(gl, 0.5);
            double flick = 0.82 + 0.18 * U.Hash(frame / 2, 9);
            DrawImageA(g, titleGlow, tx - Art.GlowPad, ty - Art.GlowPad, ua * flick);
            if (gl > 0.02)
            {
                int off = (int)(3 + 6 * gl);
                DrawImageA(g, titleRed, tx - off, ty, ua);
                DrawImageA(g, titleCyan, tx + off, ty, ua);
                int slices = 7, h = Art.TitleH / slices;
                for (int s = 0; s < slices; s++)
                {
                    int dx = (int)((U.Hash(s, frame) - 0.5) * 44 * gl);
                    int hh = s == slices - 1 ? Art.TitleH - s * h : h;
                    DrawImagePart(g, title, new Rectangle(tx + dx, ty + s * h, Art.TitleW, hh), new Rectangle(0, s * h, Art.TitleW, hh), ua);
                }
            }
            else DrawImageA(g, title, tx, ty, ua);
        }

        bool NeedsGame(int i) { return i == BtnPlay || i == BtnInstall || i == BtnRestore; }

        void DrawButton(Graphics g, int i, double t, double ua)
        {
            double appear = U.Smooth(1.62 + i * 0.07, 1.98 + i * 0.07, t);
            double a = ua * appear; if (a <= 0.01) return;
            Rectangle r = Btn[i]; r.X += (int)((1 - appear) * -30);
            bool selected = i == sel && t > 1.9 && launchAt < 0;
            bool disabled = NeedsGame(i) && !st.GameFound;
            bool armed = i == BtnRestore && restoreArmUntil > t;
            bool blink = ((int)(t * 4)) % 2 == 0;
            double age = t - selTime;

            if (selected)
            {
                using (LinearGradientBrush lb = new LinearGradientBrush(new Rectangle(r.X - 1, r.Y, r.Width + 2, r.Height), U.A(Color.FromArgb(150, 36, 128, 54), a), U.A(Color.FromArgb(70, 6, 36, 14), a), 0f))
                    g.FillRectangle(lb, r);
                for (int k = 1; k <= 4; k++)
                {
                    pen.Width = 1.5f; pen.Color = U.A(Color.FromArgb(76 - k * 16, 120, 255, 140), a);
                    Rectangle rr = r; rr.Inflate(k * 2, k * 2); g.DrawRectangle(pen, rr);
                }
            }
            else Fill(g, U.A(Color.FromArgb(135, 1, 8, 3), a), r);

            Color border = armed ? (blink ? Amber : Color.FromArgb(160, 200, 140, 60)) : selected ? Color.FromArgb(240, 175, 255, 185) : Color.FromArgb(110, 80, 140, 90);
            pen.Width = selected || armed ? 1.6f : 1f; pen.Color = U.A(border, a);
            g.DrawRectangle(pen, r);
            Fill(g, U.A(armed ? Amber : (selected ? Color.FromArgb(250, 160, 255, 170) : Color.FromArgb(120, 70, 140, 80)), a), r.X, r.Y, 4, r.Height);

            Text(g, (i + 1).ToString("00"), fIdx, U.A(Color.FromArgb(selected ? 230 : 120, 150, 230, 160), a), r.X + 15, r.Y + (i == 0 ? 11 : 9), false);
            Fill(g, U.A(Color.FromArgb(selected ? 120 : 50, 150, 230, 160), a), r.X + 15, r.Y + (i == 0 ? 27 : 25), 16, 1);

            string label = armed ? "CONFIRM UNINSTALL?" : Labels[i];
            Font lf = i == 0 ? fBtnBig : fBtn;
            float lx = r.X + 48, ly = r.Y + (i == 0 ? 12 : 5);
            Color lc = disabled ? Color.FromArgb(170, 112, 138, 114) : armed ? Amber : selected ? Hi : Color.FromArgb(225, 150, 214, 156);
            double gl = 0;
            if (selected)
            {
                if (age < 0.34) gl = 1 - age / 0.34;
                if (U.Hash(frame / 2, i + 50) > 0.982) gl = Math.Max(gl, 0.45);
            }
            if (gl > 0.02) GlitchText(g, label, lf, U.A(lc, a), lx, ly, gl, i);
            else Text(g, label, lf, U.A(lc, a), lx, ly, true);

            string sub = armed ? "PRESS AGAIN · removes the VR mod from this folder" : disabled ? "Needs OLGame.exe next to this launcher" : Subs[i];
            Color sc = armed || disabled ? Amber : (selected ? Color.FromArgb(235, 165, 240, 175) : Color.FromArgb(215, 122, 182, 130));
            Text(g, sub, fSub, U.A(sc, a), lx + 1, r.Bottom - (i == 0 ? 23 : 20), false);

            if (selected)
            {
                double swp = (t * 0.65) % 1.0; int sx = r.X + (int)(swp * (r.Width + 120)) - 60;
                Rectangle sr = new Rectangle(sx - 50, r.Y + 1, 100, r.Height - 1);
                using (LinearGradientBrush lb = new LinearGradientBrush(new Rectangle(sr.X - 1, sr.Y, sr.Width + 2, sr.Height), Color.Transparent, Color.Transparent, 0f))
                {
                    ColorBlend cb = new ColorBlend(3);
                    cb.Colors = new[] { Color.FromArgb(0, 210, 255, 215), U.A(Color.FromArgb(34, 210, 255, 215), a), Color.FromArgb(0, 210, 255, 215) };
                    cb.Positions = new[] { 0f, 0.5f, 1f };
                    lb.InterpolationColors = cb;
                    Region old = g.Clip; g.SetClip(r); g.FillRectangle(lb, sr); g.Clip = old;
                }
                if (blink || age < 0.3)
                {
                    float cx = r.Right - 26, cy = r.Y + r.Height / 2f;
                    sb.Color = U.A(Color.FromArgb(240, 180, 255, 190), a);
                    g.FillPolygon(sb, new[] { new PointF(cx + 8, cy - 8), new PointF(cx + 8, cy + 8), new PointF(cx - 2, cy) });
                }
            }
            if (pressIdx == i && t - pressTime < 0.22) Fill(g, U.A(Color.FromArgb((int)(120 * (1 - (t - pressTime) / 0.22)), 210, 255, 215), a), r);
        }

        void GlitchText(Graphics g, string s, Font f, Color c, float x, float y, double gl, int seed)
        {
            float off = (float)(2 + 5 * gl);
            sb.Color = Color.FromArgb((int)(c.A * 0.6), 255, 50, 70); g.DrawString(s, f, sb, x - off, y, sf);
            sb.Color = Color.FromArgb((int)(c.A * 0.6), 50, 255, 240); g.DrawString(s, f, sb, x + off, y, sf);
            float w = Measure(g, s, f), h = f.Size * 1.25f;
            int slices = 4; float sh = h / slices;
            Region old = g.Clip;
            for (int k = 0; k < slices; k++)
            {
                float dx = (float)((U.Hash(frame, seed * 10 + k) - 0.5) * 30 * gl);
                g.SetClip(new RectangleF(x - 60, y + k * sh, w + 120, sh + 0.5f));
                sb.Color = c; g.DrawString(s, f, sb, x + dx, y, sf);
            }
            g.Clip = old;
        }

        void DrawPanel(Graphics g, double t, double ua)
        {
            double a = ua * U.Smooth(1.85, 2.3, t); if (a < 0.01) return;
            Rectangle r = PanelR;
            Fill(g, U.A(Color.FromArgb(172, 0, 7, 3), a), r);
            pen.Width = 1; pen.Color = U.A(Color.FromArgb(140, 90, 165, 100), a); g.DrawRectangle(pen, r);
            Fill(g, U.A(Color.FromArgb(70, 90, 210, 110), a), r.X + 1, r.Y + 1, r.Width - 1, 24);
            Text(g, "SYSTEM CHECK", fStatB, U.A(Hi, a), r.X + 12, r.Y + 6, false);
            TextRight(g, "CAM 01", fStatB, U.A(NvMain, a), r.Right - 12, r.Y + 6, false);
            int y = r.Y + 34;
            foreach (Row row in rows)
            {
                Color tc = row.Kind == Tone.Ok ? NvMain : row.Kind == Tone.Warn ? Amber : row.Kind == Tone.Err ? ErrC : NvDim;
                Text(g, "[" + row.Tag + "]", fStatB, U.A(tc, a), r.X + 12, y, false);
                Text(g, row.Key, fStatB, U.A(Color.FromArgb(230, 190, 235, 195), a), r.X + 56, y, false);
                Text(g, row.Value, fStat, U.A(row.Kind == Tone.Info ? NvDim : Color.FromArgb(240, 214, 242, 216), a), r.X + 128, y, false);
                y += 19;
            }
            y += 4;
            Fill(g, U.A(Color.FromArgb(90, 110, 200, 120), a), r.X + 12, y + 7, r.Width - 24, 1);
            Fill(g, U.A(Color.FromArgb(255, 1, 9, 4), a), r.X + 14, y, 70, 14);
            Text(g, "TAPE LOG", fIdx, U.A(NvDim, a), r.X + 18, y, false);
            y += 18;
            int shown = 0, start = 0;
            for (int i = log.Count - 1; i >= 0; i--) if (log[i].At <= t) { shown++; if (shown == 5) { start = i; break; } }
            for (int i = start; i < log.Count; i++)
            {
                LogLine l = log[i]; if (l.At > t) continue;
                int n = Math.Min(l.Text.Length, (int)((t - l.At) * 90) + 1);
                Color lc = l.Kind == Tone.Ok ? NvMain : l.Kind == Tone.Warn ? Amber : l.Kind == Tone.Err ? ErrC : Color.FromArgb(220, 175, 215, 180);
                string s = "> " + l.Text.Substring(0, n) + (n < l.Text.Length ? "█" : "");
                Text(g, s, fLog, U.A(lc, a), r.X + 12, y, false);
                y += 17;
            }
        }

        void DrawCredit(Graphics g, double t, double ua)
        {
        }

        // ---------------- camcorder OSD ----------------

        void DrawOsd(Graphics g, double t)
        {
            if (t > 1.12) Corners(g, 0.9);
            if (t > 1.2)
            {
                bool rec = t >= 1.5;
                if (rec)
                {
                    double rate = launchAt >= 0 ? 3.0 : 1.0;
                    bool on = ((t - 1.5) * rate % 1.0) < 0.62;
                    if (on)
                    {
                        sb.Color = Color.FromArgb(70, 255, 40, 30); g.FillEllipse(sb, 52, 40, 23, 23);
                        sb.Color = RecRed; g.FillEllipse(sb, 56, 44, 15, 15);
                    }
                    Text(g, "REC", fRec, Osd, 80, 40, true);
                }
                else Text(g, "STBY", fRec, Osd, 56, 40, true);
                Text(g, TimeCode(Math.Max(0, t - 1.5)), fTc, Osd, 150, 41, true);
                Text(g, page == PageSettings ? "SP   TAPE 02" : page == PageControls ? "SP   TAPE 03" : "SP   TAPE 01", fOsdSm, U.A(Osd, 0.8), 58, 68, true);
            }
            if (t > 1.3)
            {
                // battery
                pen.Width = 1.6f; pen.Color = Osd;
                g.DrawRectangle(pen, 1178, 44, 36, 16);
                Fill(g, Osd, 1214, 48, 4, 8);
                for (int k = 0; k < 4; k++)
                {
                    bool full = k < 3 || ((int)(t * 1.5)) % 2 == 0;
                    if (full) Fill(g, Osd, 1181 + k * 8, 47, 6, 10);
                }
                // night vision
                sb.Color = Osd; g.FillRegion(sb, moon);
                TextRight(g, "NIGHT VISION", fOsdMid, Osd, 1168, 44, true);
                DrawSpeaker(g, t);
                // zoom bar
                Text(g, "T", fOsdSm, U.A(Osd, 0.8), 1228, 134, true);
                Text(g, "W", fOsdSm, U.A(Osd, 0.8), 1228, 326, true);
                Fill(g, U.A(Osd, 0.6), 1232, 152, 2, 170);
                for (int k = 0; k <= 10; k++) Fill(g, U.A(Osd, 0.6), 1228, 152 + k * 17, k % 5 == 0 ? 10 : 6, 1);
                float zy = (float)(300 + Math.Sin(t * 0.37) * 5);
                sb.Color = Osd; g.FillPolygon(sb, new[] { new PointF(1222, zy - 5), new PointF(1222, zy + 5), new PointF(1229, zy) });
            }
            if (t > 1.36)
            {
                DateTime d = new DateTime(2013, 10, 31, 23, 57, 13).AddSeconds(Math.Max(0, t - 1.36));
                TextRight(g, d.ToString("MMM dd yyyy", CultureInfo.InvariantCulture).ToUpperInvariant(), fOsdMid, Osd, 1222, 646, true);
                TextRight(g, d.ToString("h:mm:ss tt", CultureInfo.InvariantCulture), fTc, Osd, 1222, 664, true);
            }
            if (t > 1.45 && heavyReady && page == PageMain) DrawFocus(g, t);
        }

        static string TimeCode(double s)
        {
            int f = (int)((s % 1.0) * 30), sec = (int)s;
            return string.Format(CultureInfo.InvariantCulture, "{0:00}:{1:00}:{2:00}:{3:00}", sec / 3600, (sec / 60) % 60, sec % 60, f);
        }

        void Corners(Graphics g, double a)
        {
            pen.Width = 2.2f; pen.Color = U.A(Osd, a);
            int e = 30, arm = 44;
            g.DrawLine(pen, e, e, e + arm, e); g.DrawLine(pen, e, e, e, e + arm);
            g.DrawLine(pen, W - e, e, W - e - arm, e); g.DrawLine(pen, W - e, e, W - e, e + arm);
            g.DrawLine(pen, e, H - e, e + arm, H - e); g.DrawLine(pen, e, H - e, e, H - e - arm);
            g.DrawLine(pen, W - e, H - e, W - e - arm, H - e); g.DrawLine(pen, W - e, H - e, W - e, H - e - arm);
        }

        void DrawSpeaker(Graphics g, double t)
        {
            bool hot = hover == HitSpeaker;
            Color c = hot ? Hi : U.A(Osd, 0.9);
            if (hot) Fill(g, Color.FromArgb(50, 140, 255, 160), SpeakerR);
            int x = SpeakerR.X + 6, y = SpeakerR.Y + 6;
            sb.Color = c;
            g.FillRectangle(sb, x, y + 5, 5, 7);
            g.FillPolygon(sb, new[] { new PointF(x + 4, y + 5), new PointF(x + 11, y), new PointF(x + 11, y + 17), new PointF(x + 4, y + 12) });
            pen.Width = 1.8f; pen.Color = c;
            if (muted)
            {
                g.DrawLine(pen, x + 15, y + 4, x + 22, y + 13); g.DrawLine(pen, x + 22, y + 4, x + 15, y + 13);
            }
            else
            {
                g.DrawArc(pen, x + 8, y + 3, 9, 11, -50, 100);
                g.DrawArc(pen, x + 8, y - 1, 15, 19, -50, 100);
            }
        }

        void DrawFocus(Graphics g, double t)
        {
            int sx = (int)Math.Round(Math.Sin(t * 0.61) * 5 + Math.Sin(t * 1.73 + 1.3) * 2);
            int sy = (int)Math.Round(Math.Sin(t * 0.47 + 2.1) * 4 + Math.Sin(t * 1.29) * 1.5);
            double c = t % 11.0;
            double hunt = c < 1.3 ? Math.Sin(c * 14) * 0.08 * (1 - c / 1.3) : 0;
            float cx = focusBg.X - 20 + sx, cy = focusBg.Y - 20 + sy;
            float w = (float)(118 * (1 + hunt)), h = (float)(150 * (1 + hunt));
            float l = cx - w / 2, r = cx + w / 2, tp = cy - h / 2, b = cy + h / 2, arm = 14;
            pen.Width = 1.6f; pen.Color = U.A(Osd, 0.55);
            g.DrawLine(pen, l, tp, l + arm, tp); g.DrawLine(pen, l, tp, l, tp + arm);
            g.DrawLine(pen, r, tp, r - arm, tp); g.DrawLine(pen, r, tp, r, tp + arm);
            g.DrawLine(pen, l, b, l + arm, b); g.DrawLine(pen, l, b, l, b - arm);
            g.DrawLine(pen, r, b, r - arm, b); g.DrawLine(pen, r, b, r, b - arm);
            Text(g, "AF", fOsdSm, U.A(Osd, 0.5), r + 6, tp - 2, true);
        }

        void DrawWindowButtons(Graphics g)
        {
            bool hm = hover == HitMin, hc = hover == HitClose;
            if (hm) Fill(g, Color.FromArgb(70, 140, 255, 160), MinR);
            if (hc) Fill(g, Color.FromArgb(150, 200, 30, 25), CloseR);
            pen.Width = 1.8f;
            pen.Color = hm ? Hi : Color.FromArgb(170, 200, 230, 200);
            g.DrawLine(pen, MinR.X + 11, MinR.Y + 15, MinR.Right - 11, MinR.Y + 15);
            pen.Color = hc ? Color.White : Color.FromArgb(170, 200, 230, 200);
            int cx = CloseR.X + CloseR.Width / 2, cy = CloseR.Y + CloseR.Height / 2;
            g.DrawLine(pen, cx - 6, cy - 6, cx + 6, cy + 6); g.DrawLine(pen, cx + 6, cy - 6, cx - 6, cy + 6);
        }

        void DrawPowerOn(Graphics g, double t)
        {
            if (t < 1.03)
            {
                g.Clear(Color.Black);
                double k = U.Smooth(0.95, 1.03, t);
                int w = (int)(W * k);
                Fill(g, Color.FromArgb(255, 220, 255, 225), (W - w) / 2, H / 2 - 1, w, 2);
                Fill(g, Color.FromArgb(60, 150, 255, 170), (W - w) / 2, H / 2 - 5, w, 10);
                return;
            }
            double o = U.Smooth(1.03, 1.30, t);
            int hh = (int)(H * o);
            int top = (H - hh) / 2;
            if (top > 0)
            {
                Fill(g, Color.Black, 0, 0, W, top);
                Fill(g, Color.Black, 0, top + hh, W, H - top - hh);
                Fill(g, Color.FromArgb((int)(200 * (1 - o)), 220, 255, 225), 0, top, W, 2);
                Fill(g, Color.FromArgb((int)(200 * (1 - o)), 220, 255, 225), 0, top + hh - 2, W, 2);
            }
            double flash = 1 - U.Smooth(1.03, 1.65, t);
            if (flash > 0) Fill(g, Color.FromArgb((int)(115 * flash * flash), 215, 255, 220), 0, Math.Max(0, top), W, Math.Min(H, hh));
        }

        void DrawLaunch(Graphics g, double lt)
        {
            double p = 0;
            double[] beats = { 0.05, 0.90, 1.62 };
            foreach (double b in beats)
            {
                if (lt >= b) p += Math.Exp(-(lt - b) / 0.16);
                if (lt >= b + 0.24) p += 0.6 * Math.Exp(-(lt - b - 0.24) / 0.14);
            }
            p = Math.Min(1.4, p);
            Fill(g, Color.FromArgb((int)Math.Min(200, 70 * U.Smooth(0, 1.4, lt) + 50 * p), 0, 0, 0), 0, 0, W, H);
            using (GraphicsPath gp = new GraphicsPath())
            {
                gp.AddEllipse(-260, -200, W + 520, H + 400);
                using (PathGradientBrush pb = new PathGradientBrush(gp))
                {
                    pb.CenterColor = Color.FromArgb(0, 0, 0, 0);
                    pb.SurroundColors = new[] { Color.FromArgb((int)Math.Min(255, 120 + 120 * p), 0, 0, 0) };
                    pb.FocusScales = new PointF(0.35f, 0.35f);
                    g.FillRectangle(pb, 0, 0, W, H);
                }
            }
            string l1 = "PUT ON YOUR HEADSET";
            int n = Math.Min(l1.Length, (int)(Math.Max(0, lt - 0.15) * 34));
            if (n > 0) TextCenter(g, l1.Substring(0, n), fCenterBig, Color.FromArgb(250, 215, 255, 220), W / 2f, 300, true);
            if (lt > 0.8)
            {
                int dots = ((int)(lt * 4)) % 4;
                TextCenter(g, "STARTING OUTLAST VR" + new string('.', dots) + new string(' ', 3 - dots), fCenter, Color.FromArgb(230, 150, 235, 160), W / 2f, 352, true);
            }
        }

        void DrawCollapse(Graphics g, double c)
        {
            g.Flush(FlushIntention.Sync);
            if (tmpBuf == null) tmpBuf = new Bitmap(W, H, PixelFormat.Format32bppPArgb);
            using (Graphics tg = Graphics.FromImage(tmpBuf)) { tg.CompositingMode = CompositingMode.SourceCopy; tg.DrawImage(outBuf, 0, 0, W, H); }
            g.Clear(Color.Black);
            if (c < 0.65)
            {
                double k = c / 0.65;
                int h = Math.Max(2, (int)(H * Math.Pow(1 - k, 2.2)));
                g.InterpolationMode = InterpolationMode.Bilinear;
                g.DrawImage(tmpBuf, new Rectangle(0, (H - h) / 2, W, h), new Rectangle(0, 0, W, H), GraphicsUnit.Pixel);
                g.InterpolationMode = InterpolationMode.NearestNeighbor;
                Fill(g, Color.FromArgb((int)(200 * k), 225, 255, 230), 0, (H - h) / 2, W, h);
            }
            else
            {
                double k = (c - 0.65) / 0.35;
                int w = Math.Max(2, (int)(W * Math.Pow(1 - k, 2)));
                Fill(g, Color.FromArgb(70, 150, 255, 170), (W - w) / 2, H / 2 - 5, w, 10);
                Fill(g, Color.FromArgb(255, 230, 255, 235), (W - w) / 2, H / 2 - 1, w, 2);
                if (k > 0.7)
                {
                    sb.Color = Color.FromArgb((int)(255 * (1 - k) / 0.3), 230, 255, 235);
                    g.FillEllipse(sb, W / 2f - 5, H / 2f - 5, 10, 10);
                }
            }
        }

        public void Dispose()
        {
            if (sfx != null) { sfx.Dispose(); sfx = null; }
        }
    }

    // ---------------------------------------------------------------------------------
    //  XInput (controller support, only polled while the launcher has focus)
    // ---------------------------------------------------------------------------------
    static class Pad
    {
        [StructLayout(LayoutKind.Sequential)]
        public struct State { public uint Packet; public ushort Buttons; public byte LT, RT; public short LX, LY, RX, RY; }

        [DllImport("xinput1_4.dll", EntryPoint = "XInputGetState")] static extern int Get14(int i, out State s);
        [DllImport("xinput9_1_0.dll", EntryPoint = "XInputGetState")] static extern int Get910(int i, out State s);

        static int mode;   // 0 = xinput1_4, 1 = xinput9_1_0, 2 = unavailable

        public static bool Read(int i, out State s)
        {
            s = new State();
            if (mode == 0) { try { return Get14(i, out s) == 0; } catch { mode = 1; } }
            if (mode == 1) { try { return Get910(i, out s) == 0; } catch { mode = 2; } }
            return false;
        }

        public const ushort Up = 0x0001, Down = 0x0002, Left = 0x0004, Right = 0x0008, Start = 0x0010, A = 0x1000, B = 0x2000;
    }

    // ---------------------------------------------------------------------------------
    //  The borderless window
    // ---------------------------------------------------------------------------------
    sealed class LauncherForm : Form
    {
        [DllImport("user32.dll")] static extern bool ReleaseCapture();
        [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, int msg, IntPtr w, IntPtr l);
        // [PRESENT] The picture reaches the window through GDI: each frame is copied into a DIB section and
        // StretchBlt'd - HALFTONE when it is scaled, a plain copy at 1:1. Scaling with GDI+ DrawImage (bilinear)
        // cost 75 ms at 1600x900 and 170 ms at 2400x1350 per frame on this kind of PC - the launcher crawled at
        // 6-10 fps full screen and on a 125% display; HALFTONE costs 10-18 ms for the same picture.
        [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleDC(IntPtr dc);
        [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr dc, IntPtr o);
        [DllImport("gdi32.dll")] static extern bool DeleteDC(IntPtr dc);
        [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr o);
        [DllImport("gdi32.dll")] static extern int SetStretchBltMode(IntPtr dc, int mode);
        [DllImport("gdi32.dll")] static extern bool SetBrushOrgEx(IntPtr dc, int x, int y, IntPtr p);
        [DllImport("gdi32.dll")] static extern bool StretchBlt(IntPtr d, int dx, int dy, int dw, int dh, IntPtr s, int sx, int sy, int sw, int sh, int rop);
        [DllImport("gdi32.dll")] static extern bool PatBlt(IntPtr dc, int x, int y, int w, int h, int rop);
        [StructLayout(LayoutKind.Sequential)] struct BIH { public int size, w, h; public short planes, bpp; public int comp, img, xppm, yppm, used, imp; }
        [DllImport("gdi32.dll")] static extern IntPtr CreateDIBSection(IntPtr dc, ref BIH bi, int usage, out IntPtr bits, IntPtr sec, int off);
        [DllImport("kernel32.dll", EntryPoint = "RtlMoveMemory")] static extern void MoveMem(IntPtr dst, IntPtr src, IntPtr n);
        const int SRCCOPY = 0x00CC0020, BLACKNESS = 0x00000042, COLORONCOLOR = 3, HALFTONE = 4;
        IntPtr dibDc = IntPtr.Zero, dib = IntPtr.Zero, dibOld = IntPtr.Zero, dibBits = IntPtr.Zero;
        bool dibFailed;

        readonly Viewfinder vf;
        readonly System.Windows.Forms.Timer timer = new System.Windows.Forms.Timer();
        float scale = 1f;
        // FULL SCREEN (the default): the window covers the whole monitor and the 1280x720 picture is
        // scaled to fit it, black bars where the monitor is not 16:9. F11 or Alt+Enter switches; the
        // choice is remembered in %LOCALAPPDATA%\OutlastVR\launcher.ini (--fullscreen / --windowed).
        bool full;
        float winScale = 1f;
        Point offs = Point.Empty;
        Size drawSize = new Size(Viewfinder.W, Viewfinder.H);

        static string PrefsPath() { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "OutlastVR", "launcher.ini"); }
        static bool LoadFullscreen()
        {
            try { foreach (string l in File.ReadAllLines(PrefsPath())) if (l.Trim().Equals("Fullscreen=0", StringComparison.OrdinalIgnoreCase)) return false; }
            catch { }
            return true;   // nothing remembered: full screen
        }
        static void SaveFullscreen(bool on)
        {
            try { Directory.CreateDirectory(Path.GetDirectoryName(PrefsPath())); File.WriteAllText(PrefsPath(), "[Launcher]\r\nFullscreen=" + (on ? "1" : "0") + "\r\n"); }
            catch { }
        }

        void SetFullscreen(bool on, bool remember)
        {
            full = on;
            WindowState = FormWindowState.Normal;
            if (on)
            {
                Rectangle b = Screen.FromControl(this).Bounds;
                Bounds = b;
                scale = Math.Min(b.Width / (float)Viewfinder.W, b.Height / (float)Viewfinder.H);
                drawSize = new Size((int)Math.Round(Viewfinder.W * scale), (int)Math.Round(Viewfinder.H * scale));
                offs = new Point((b.Width - drawSize.Width) / 2, (b.Height - drawSize.Height) / 2);
            }
            else
            {
                scale = winScale;
                drawSize = new Size((int)(Viewfinder.W * scale), (int)(Viewfinder.H * scale));
                offs = Point.Empty;
                ClientSize = drawSize;
                Rectangle wa = Screen.FromControl(this).WorkingArea;
                Location = new Point(wa.Left + (wa.Width - Width) / 2, wa.Top + (wa.Height - Height) / 2);
            }
            if (remember) SaveFullscreen(on);
            Invalidate();
        }
        bool EnsureDib(IntPtr hdc)
        {
            if (dibDc != IntPtr.Zero) return true;
            if (dibFailed) return false;
            BIH bi = new BIH(); bi.size = 40; bi.w = Viewfinder.W; bi.h = -Viewfinder.H; bi.planes = 1; bi.bpp = 32;   // top-down BGRA
            dib = CreateDIBSection(hdc, ref bi, 0, out dibBits, IntPtr.Zero, 0);
            if (dib == IntPtr.Zero || dibBits == IntPtr.Zero) { dib = IntPtr.Zero; dibFailed = true; return false; }
            dibDc = CreateCompatibleDC(hdc);
            if (dibDc == IntPtr.Zero) { DeleteObject(dib); dib = IntPtr.Zero; dibFailed = true; return false; }
            dibOld = SelectObject(dibDc, dib);
            return true;
        }

        void FreeDib()
        {
            if (dibDc != IntPtr.Zero) { SelectObject(dibDc, dibOld); DeleteDC(dibDc); dibDc = IntPtr.Zero; }
            if (dib != IntPtr.Zero) { DeleteObject(dib); dib = IntPtr.Zero; }
        }

        bool PresentGdi(Graphics g, Bitmap b)
        {
            IntPtr hdc = g.GetHdc();
            try
            {
                if (!EnsureDib(hdc)) return false;
                int row = Viewfinder.W * 4;
                BitmapData d = b.LockBits(new Rectangle(0, 0, Viewfinder.W, Viewfinder.H), ImageLockMode.ReadOnly, PixelFormat.Format32bppPArgb);
                try
                {
                    if (d.Stride == row) MoveMem(dibBits, d.Scan0, (IntPtr)(row * Viewfinder.H));
                    else for (int y = 0; y < Viewfinder.H; y++) MoveMem(dibBits + y * row, d.Scan0 + y * d.Stride, (IntPtr)row);
                }
                finally { b.UnlockBits(d); }
                Size c = ClientSize;
                if (offs.X > 0) { PatBlt(hdc, 0, 0, offs.X, c.Height, BLACKNESS); PatBlt(hdc, offs.X + drawSize.Width, 0, c.Width - offs.X - drawSize.Width, c.Height, BLACKNESS); }
                if (offs.Y > 0) { PatBlt(hdc, 0, 0, c.Width, offs.Y, BLACKNESS); PatBlt(hdc, 0, offs.Y + drawSize.Height, c.Width, c.Height - offs.Y - drawSize.Height, BLACKNESS); }
                bool one = drawSize.Width == Viewfinder.W && drawSize.Height == Viewfinder.H;
                SetStretchBltMode(hdc, one ? COLORONCOLOR : HALFTONE);
                if (!one) SetBrushOrgEx(hdc, 0, 0, IntPtr.Zero);
                return StretchBlt(hdc, offs.X, offs.Y, drawSize.Width, drawSize.Height, dibDc, 0, 0, Viewfinder.W, Viewfinder.H, SRCCOPY);
            }
            finally { g.ReleaseHdc(hdc); }
        }

        // --perf-log: frame timings of the real window, written after 8 s, then the window closes
        readonly string perfPath;
        readonly Stopwatch perfClock = new Stopwatch();
        readonly List<double> perfGaps = new List<double>();
        double perfRender, perfPresent, perfLast = -1; int perfN, perfGdi;

        void PerfFrame(double t0, double t1, double t2, bool gdi)
        {
            if (perfLast >= 0) perfGaps.Add(t0 - perfLast);
            perfLast = t0; perfN++; perfRender += t1 - t0; perfPresent += t2 - t1; if (gdi) perfGdi++;
            if (t0 < 8000 || perfPath == null) return;
            string path = perfPath; perfLast = -1;
            List<double> gaps = new List<double>(perfGaps); gaps.Sort();
            double gavg = 0; foreach (double x in gaps) gavg += x; gavg /= Math.Max(1, gaps.Count);
            try
            {
                File.WriteAllText(path, string.Format(CultureInfo.InvariantCulture,
                    "{0}: client {1}x{2}, picture {3}x{4} at {5:0.000}x, {6}\r\nframes {7} in {8:0.0} s = {9:0.0} fps (frame gap avg {10:0.0} ms, median {11:0.0}, p95 {12:0.0}, max {13:0.0})\r\nrender avg {14:0.0} ms, present avg {15:0.0} ms\r\n",
                    full ? "FULL SCREEN" : "WINDOW", ClientSize.Width, ClientSize.Height, drawSize.Width, drawSize.Height, scale,
                    perfGdi == perfN ? "GDI present" : ("GDI+ fallback " + (perfN - perfGdi) + " frames"),
                    perfN, t0 / 1000.0, perfN / (t0 / 1000.0), gavg, gaps[gaps.Count / 2], gaps[(int)(gaps.Count * 0.95)], gaps[gaps.Count - 1],
                    perfRender / perfN, perfPresent / perfN));
            }
            catch { }
            BeginInvoke((MethodInvoker)Close);
        }

        int downHit = Viewfinder.HitNone;
        int padSlot = -1, padScan;
        ushort padPrev; double padRepeatAt, padHRepeatAt; int padDir, padHDir;
        readonly Stopwatch padClock = Stopwatch.StartNew();

        public LauncherForm(string gameDir, int fullscreen, string perfLog)
        {
            perfPath = perfLog;
            Text = "Outlast VR - Beta 1.0";
            FormBorderStyle = FormBorderStyle.None;
            StartPosition = FormStartPosition.CenterScreen;
            AutoScaleMode = AutoScaleMode.None;
            BackColor = Color.Black;
            KeyPreview = true;
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.Opaque, true);
            SetStyle(ControlStyles.OptimizedDoubleBuffer, false);
            Rectangle wa = Screen.PrimaryScreen.WorkingArea;
            // windowed: the size it always had - 1280x720 at 100% display scaling, scaled with the display
            float dpi = 1f;
            try { using (Graphics dg = CreateGraphics()) dpi = Math.Max(1f, dg.DpiX / 96f); } catch { }
            scale = winScale = Math.Min(dpi, Math.Min(wa.Width * 0.98f / Viewfinder.W, wa.Height * 0.98f / Viewfinder.H));
            ClientSize = drawSize = new Size((int)(Viewfinder.W * scale), (int)(Viewfinder.H * scale));
            bool startFull = fullscreen >= 0 ? fullscreen == 1 : LoadFullscreen();
            if (fullscreen >= 0 && perfPath == null) SaveFullscreen(startFull);
            if (startFull) Load += delegate { SetFullscreen(true, false); };
            Icon ico = IconMaker.Make(); if (ico != null) Icon = ico;

            vf = new Viewfinder(false, gameDir, false);
            vf.RequestClose = delegate { BeginInvoke((MethodInvoker)Close); };
            vf.RequestMinimize = delegate { WindowState = FormWindowState.Minimized; };
            if (perfPath != null) vf.MuteForSession();

            timer.Interval = 30;
            timer.Tick += delegate
            {
                vf.Tick();
                PollPad();
                if (WindowState != FormWindowState.Minimized) Invalidate();
            };
            Shown += delegate { vf.StartClock(); vf.StartAudio(); timer.Start(); Activate(); perfClock.Start(); };
            FormClosed += delegate { timer.Stop(); vf.Dispose(); FreeDib(); };
        }

        protected override CreateParams CreateParams
        {
            get
            {
                CreateParams cp = base.CreateParams;
                cp.ClassStyle |= 0x00020000;   // CS_DROPSHADOW
                cp.Style |= 0x00020000;        // WS_MINIMIZEBOX (taskbar minimise/restore)
                return cp;
            }
        }

        Point Logical(Point p)
        {
            if (!full && scale >= 0.999f && scale <= 1.001f) return p;
            return new Point((int)((p.X - offs.X) / scale), (int)((p.Y - offs.Y) / scale));
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            double t0 = perfClock.Elapsed.TotalMilliseconds;
            Bitmap b = vf.Render();
            double t1 = perfClock.Elapsed.TotalMilliseconds;
            bool gdi = PresentGdi(e.Graphics, b);
            if (!gdi)
            {
                // fallback: GDI+ (nearest neighbour - its bilinear is far too slow to scale every frame)
                Graphics g = e.Graphics;
                g.CompositingMode = CompositingMode.SourceCopy;
                if (offs.X > 0 || offs.Y > 0) g.Clear(Color.Black);                   // letterbox bars
                g.InterpolationMode = InterpolationMode.NearestNeighbor;
                g.DrawImage(b, offs.X, offs.Y, drawSize.Width, drawSize.Height);
            }
            if (perfPath != null && perfClock.IsRunning) PerfFrame(t0, t1, perfClock.Elapsed.TotalMilliseconds, gdi);
        }

        protected override void OnPaintBackground(PaintEventArgs e) { }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            base.OnMouseMove(e);
            bool hot = vf.MouseMove(Logical(e.Location));
            Cursor = hot ? Cursors.Hand : Cursors.Default;
        }

        protected override void OnMouseLeave(EventArgs e) { base.OnMouseLeave(e); vf.MouseMove(new Point(-100, -100)); }

        protected override void OnMouseWheel(MouseEventArgs e) { base.OnMouseWheel(e); vf.Wheel(e.Delta); }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            base.OnMouseDown(e);
            if (e.Button != MouseButtons.Left) return;
            Point p = Logical(e.Location);
            int hit = vf.HitTest(p);
            if (vf.Booting && hit != Viewfinder.HitClose && hit != Viewfinder.HitMin) { vf.SkipBoot(); downHit = Viewfinder.HitNone; return; }
            if (hit == Viewfinder.HitNone)
            {
                downHit = Viewfinder.HitNone;
                if (full) return;                                                     // a full-screen window stays put
                ReleaseCapture();
                SendMessage(Handle, 0xA1, (IntPtr)2, IntPtr.Zero);   // WM_NCLBUTTONDOWN, HTCAPTION: drag the window
                return;
            }
            downHit = hit;
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            base.OnMouseUp(e);
            if (e.Button != MouseButtons.Left) return;
            int hit = vf.HitTest(Logical(e.Location));
            if (hit != Viewfinder.HitNone && hit == downHit) vf.Click(hit);
            downHit = Viewfinder.HitNone;
        }

        protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
        {
            Keys k = keyData & Keys.KeyCode;
            if (k == Keys.F11 || (k == Keys.Enter && (keyData & Keys.Alt) != 0)) { SetFullscreen(!full, true); return true; }
            switch (k)
            {
                case Keys.Escape: case Keys.Up: case Keys.Down: case Keys.Tab: case Keys.Enter: case Keys.Space:
                case Keys.W: case Keys.S: case Keys.M: case Keys.Home: case Keys.End:
                case Keys.Left: case Keys.Right: case Keys.A: case Keys.D: case Keys.Back: case Keys.PageUp: case Keys.PageDown:
                    vf.HandleKey(k, (keyData & Keys.Shift) != 0); return true;
            }
            if (vf.Booting && k != Keys.ShiftKey && k != Keys.ControlKey && k != Keys.Menu) { vf.SkipBoot(); return true; }
            return base.ProcessCmdKey(ref msg, keyData);
        }

        void PollPad()
        {
            if (Form.ActiveForm != this) { padPrev = 0; return; }
            Pad.State s;
            if (padSlot < 0)
            {
                if (padScan-- > 0) return;
                padScan = 60;
                for (int i = 0; i < 4; i++) if (Pad.Read(i, out s)) { padSlot = i; break; }
                if (padSlot < 0) return;
            }
            if (!Pad.Read(padSlot, out s)) { padSlot = -1; padScan = 60; return; }
            ushort b = s.Buttons, pressed = (ushort)(b & ~padPrev);
            padPrev = b;
            double now = padClock.Elapsed.TotalSeconds;
            if (vf.Booting) { if (pressed != 0) vf.SkipBoot(); return; }
            int dir = (b & Pad.Up) != 0 || s.LY > 16000 ? -1 : ((b & Pad.Down) != 0 || s.LY < -16000 ? 1 : 0);
            if (dir != 0 && (dir != padDir || now >= padRepeatAt)) { vf.Move(dir); padRepeatAt = now + (dir != padDir ? 0.38 : 0.16); }
            padDir = dir;
            // left / right: change a setting, step through the controls
            int hdir = (b & Pad.Left) != 0 || s.LX < -16000 ? -1 : ((b & Pad.Right) != 0 || s.LX > 16000 ? 1 : 0);
            if (hdir != 0 && (hdir != padHDir || now >= padHRepeatAt)) { vf.MoveH(hdir); padHRepeatAt = now + (hdir != padHDir ? 0.38 : 0.16); }
            padHDir = hdir;
            if ((pressed & (Pad.A | Pad.Start)) != 0) vf.HandleKey(Keys.Enter, false);
            if ((pressed & Pad.B) != 0) vf.PadBack();
        }
    }

    // ---------------------------------------------------------------------------------
    //  Developer tools (no sound, nothing written except the requested output)
    // ---------------------------------------------------------------------------------
    static class DevTools
    {
        public static int Run(Opts o)
        {
            if (o.Preview != null) return Preview(o);
            if (o.Bench != null) return Bench(o);
            if (o.IniTest != null) return IniTest(o);
            if (o.RestoreTest != null) return RestoreTest(o);
            if (o.DumpAudio != null) return DumpAudio(o);
            if (o.WriteIcon != null) { IconMaker.WriteIco(o.WriteIcon); return 0; }
            if (o.SysInfo != null) return Sys(o);
            if (o.SelfTest != null) return SelfTest(o);
            if (o.SettingsTest != null)
            {
                using (Viewfinder v = new Viewfinder(true, Path.GetFullPath(o.SettingsTest), true)) File.WriteAllText(Path.Combine(Path.GetFullPath(o.SettingsTest), "settings_test.txt"), v.SettingsTest());
                return 0;
            }
            return 1;
        }

        static string GameDir(Opts o) { return o.GameDir != null ? Path.GetFullPath(o.GameDir) : Program.AppDir(); }

        // --preview out.png [--frame N] [--hover I] [--hover-age S] [--procedural] [--gamedir DIR]
        //           [--launch-frame N] [--exit-frame N] [--restore-armed] [--dry-install]
        static int Preview(Opts o)
        {
            using (Viewfinder v = new Viewfinder(true, GameDir(o), o.Procedural))
            {
                double t = o.Frame / 30.0;
                if (o.LaunchFrame >= 0 || o.ExitFrame >= 0) t = Math.Max(t, 4.0);
                v.SetPreview(t, o.Hover, o.HoverAge);
                if (o.DryInstall) v.PreviewDryInstall(t - 1.5);
                if (o.RestoreArmed) { v.SetPreview(t, Viewfinder.BtnRestore, o.HoverAge); v.PreviewArmRestore(t); }
                if (o.Page > 0) v.PreviewPage(o.Page, o.PageSel, o.PageAge);
                if (o.LaunchFrame >= 0) { t += o.LaunchFrame / 30.0; v.SetPreview(t, 0, 3); v.PreviewLaunch(t, o.LaunchFrame / 30.0); }
                if (o.ExitFrame >= 0) { t += o.ExitFrame / 30.0; v.SetPreview(t, Viewfinder.BtnExit, 3); v.PreviewExit(t, o.ExitFrame / 30.0); }
                Bitmap b = v.Render();
                U.SavePng(b, o.Preview);
                if (v.LastError != null) File.WriteAllText(Path.GetFullPath(o.Preview) + ".error.txt", v.LastError);
            }
            return 0;
        }

        static int Bench(Opts o)
        {
            Stopwatch s = Stopwatch.StartNew();
            using (Viewfinder v = new Viewfinder(true, GameDir(o), o.Procedural))
            {
                double init = s.Elapsed.TotalMilliseconds;
                List<double> ms = new List<double>();
                for (int i = 0; i < 150; i++)
                {
                    v.SetPreview(2.5 + i / 30.0, 0, 1.0 + i / 30.0);
                    Stopwatch f = Stopwatch.StartNew(); v.Render(); ms.Add(f.Elapsed.TotalMilliseconds);
                }
                ms.Sort();
                double avg = 0; foreach (double m in ms) avg += m; avg /= ms.Count;
                StringBuilder pr = new StringBuilder();
                string[] names = { "scene", "ui", "band", "noise", "overlay", "roll+head", "osd" };
                for (int i = 0; i < names.Length; i++) pr.AppendFormat(CultureInfo.InvariantCulture, "{0} {1:0.0} ms\r\n", names[i], Viewfinder.Prof[i] / ms.Count);
                File.WriteAllText(o.Bench, string.Format(CultureInfo.InvariantCulture,
                    "init {0:0} ms\r\nframe avg {1:0.0} ms, median {2:0.0} ms, p95 {3:0.0} ms, max {4:0.0} ms\r\nerror: {5}\r\n",
                    init, avg, ms[ms.Count / 2], ms[(int)(ms.Count * 0.95)], ms[ms.Count - 1], v.LastError ?? "none") + pr.ToString());
            }
            return 0;
        }

        // --ini-test DIR [--res WxH]: run the install edits on copies of the ini files in DIR
        static int IniTest(Opts o)
        {
            List<Msg> m = new List<Msg>();
            bool ok = Installer.Install(Path.GetFullPath(o.IniTest), o.ResX, o.ResY, m, false);
            StringBuilder sb = new StringBuilder();
            sb.AppendLine("ok=" + ok);
            foreach (Msg x in m) sb.AppendLine(x.ToString());
            File.WriteAllText(Path.Combine(Path.GetFullPath(o.IniTest), "_install_report.txt"), sb.ToString());
            return ok ? 0 : 1;
        }

        // --restore-test CFGDIR --gamedir FAKEGAMEDIR
        static int RestoreTest(Opts o)
        {
            List<Msg> m = new List<Msg>();
            bool ok = Installer.Restore(Path.GetFullPath(o.RestoreTest), GameDir(o), m, false);
            StringBuilder sb = new StringBuilder();
            sb.AppendLine("ok=" + ok);
            foreach (Msg x in m) sb.AppendLine(x.ToString());
            File.WriteAllText(Path.Combine(Path.GetFullPath(o.RestoreTest), "_restore_report.txt"), sb.ToString());
            return ok ? 0 : 1;
        }

        static int DumpAudio(Opts o)
        {
            string d = Path.GetFullPath(o.DumpAudio);
            Directory.CreateDirectory(d);
            File.WriteAllBytes(Path.Combine(d, "boot.wav"), Synth.ToWav(Synth.BootSequence()));
            File.WriteAllBytes(Path.Combine(d, "ambient.wav"), Synth.ToWav(Synth.Ambient()));
            File.WriteAllBytes(Path.Combine(d, "tick.wav"), Synth.ToWav(Synth.Tick(1)));
            File.WriteAllBytes(Path.Combine(d, "confirm.wav"), Synth.ToWav(Synth.Confirm()));
            File.WriteAllBytes(Path.Combine(d, "success.wav"), Synth.ToWav(Synth.Success()));
            File.WriteAllBytes(Path.Combine(d, "error.wav"), Synth.ToWav(Synth.Error()));
            File.WriteAllBytes(Path.Combine(d, "armed.wav"), Synth.ToWav(Synth.Armed()));
            File.WriteAllBytes(Path.Combine(d, "heartbeat.wav"), Synth.ToWav(Synth.Heartbeat()));
            File.WriteAllBytes(Path.Combine(d, "poweroff.wav"), Synth.ToWav(Synth.PowerOff()));
            File.WriteAllBytes(Path.Combine(d, "ghost.wav"), Synth.ToWav(Synth.GhostCue()));
            return 0;
        }

        // --selftest report.txt [--gamedir DIR]: exercises the mixer (muted: silent), the form constructor
        // (never shown) and the menu actions on a preview viewfinder (dry run: nothing is written or started).
        static int SelfTest(Opts o)
        {
            StringBuilder r = new StringBuilder();
            try
            {
                using (Mixer m = new Mixer())
                {
                    m.Muted = true;
                    Thread.Sleep(120);
                    int id = m.Play(Synth.BootSequence(), 1f, false, 0);
                    Thread.Sleep(300);
                    bool playing = m.IsPlaying(id);
                    m.Stop(id, 0.02);
                    Thread.Sleep(250);
                    r.AppendLine("mixer ok=" + m.Ok + " playingAfter300ms=" + playing + " stoppedAfterFade=" + !m.IsPlaying(id));
                }
            }
            catch (Exception ex) { r.AppendLine("mixer EXCEPTION " + ex); }
            try
            {
                // -1: the remembered choice, read only - a 0 here saved "windowed" over the user's own choice
                using (LauncherForm f = new LauncherForm(GameDir(o), -1, null)) { r.AppendLine("form constructed, client=" + f.ClientSize); }
            }
            catch (Exception ex) { r.AppendLine("form EXCEPTION " + ex); }
            try
            {
                using (Viewfinder v = new Viewfinder(true, GameDir(o), false))
                {
                    v.SetPreview(5, 0, 2);
                    v.HandleKey(Keys.Down, false); v.HandleKey(Keys.Up, false);
                    v.Activate(Viewfinder.BtnInstall); v.SetPreview(6, Viewfinder.BtnInstall, 2);
                    v.Activate(Viewfinder.BtnRestore); v.SetPreview(6.5, Viewfinder.BtnRestore, 2); v.Activate(Viewfinder.BtnRestore); v.SetPreview(7, Viewfinder.BtnRestore, 2);
                    v.Activate(Viewfinder.BtnSettings); v.SetPreview(8, Viewfinder.BtnSettings, 2);
                    v.HandleKey(Keys.Down, false); v.HandleKey(Keys.Down, false); v.HandleKey(Keys.Escape, false);
                    r.AppendLine("settings page closed back to main: " + (v.Page == Viewfinder.PageMain));
                    v.Activate(Viewfinder.BtnControls); v.SetPreview(9, Viewfinder.BtnControls, 2); v.Render();
                    v.HandleKey(Keys.Right, false); v.HandleKey(Keys.Escape, false);
                    r.AppendLine("controls page closed back to main: " + (v.Page == Viewfinder.PageMain));
                    v.ToggleMute(); v.ToggleMute();
                    v.Render();
                    r.AppendLine("viewfinder render error=" + (v.LastError ?? "none"));
                    foreach (string s in v.LogTexts()) r.AppendLine("  log: " + s);
                    U.SavePng(v.Render(), o.SelfTest + ".png");
                }
            }
            catch (Exception ex) { r.AppendLine("viewfinder EXCEPTION " + ex); }
            File.WriteAllText(o.SelfTest, r.ToString());
            return 0;
        }

        static int Sys(Opts o)
        {
            StringBuilder sb = new StringBuilder();
            Size p = SysInfo.PhysicalPrimary();
            sb.AppendLine("physical=" + p.Width + "x" + p.Height);
            sb.AppendLine("screenBounds=" + Screen.PrimaryScreen.Bounds.Width + "x" + Screen.PrimaryScreen.Bounds.Height);
            sb.AppendLine("config=" + SysInfo.ConfigDir());
            sb.AppendLine("logdir=" + SysInfo.LogDir());
            sb.AppendLine("build=" + SysInfo.ReadBuild());
            sb.AppendLine("dllbuild=" + SysInfo.FriendlyBuild(SysInfo.ReadDllBuild(GameDir(o))));
            string rt = SysInfo.ActiveRuntime();
            sb.AppendLine("runtime=" + rt + " -> " + SysInfo.FriendlyRuntime(rt));
            sb.AppendLine("splash=" + SysInfo.FindSplash(GameDir(o)));
            File.WriteAllText(o.SysInfo, sb.ToString());
            return 0;
        }
    }
}
