// =====================================================================================
//  Outlast1VR_Pages.cs  -  the launcher's second and third tapes (Test 38)
//
//  TAPE 02  VR SETTINGS       the mod's own settings (outlastvr.ini next to the launcher), changed
//                             with the keyboard, mouse or a controller and saved the moment they change.
//  TAPE 03  QUEST 3 CONTROLS  both Touch Plus controllers, drawn from Meta's own product photos
//                             (round grey face, thumbstick, two face buttons, Menu / Meta button, the
//                             trigger under the front of the face, the grip on the inner side of the
//                             handle), every control named in words, and the hand gestures below.
//
//  Same look as the rest of the launcher: night-vision green, the camcorder OSD, scan sweeps,
//  typed labels and a burst of tape static when you change tapes.
// =====================================================================================

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

namespace Outlast1VR
{
    enum SetKind { Toggle, Choice, Number, Action }

    sealed class SettingDef
    {
        public string Group, Section, Key, Label, Help;
        public SetKind Kind;
        public string[] Names;            // Choice: what each value is called
        public double[] Values;           // Choice: the values written
        public double Min, Max, Step, Def;
        public string Unit;               // Number: "%" shows value*100, "cm" shows value*100 cm
        public string Off;                // Number: what 0 is called ("OFF"), or null
        public int Action;                // Action: 1 reset how-to labels, 2 open the log folder
    }

    // outlastvr.ini, read and written the way the mod itself does (GetPrivateProfileString).
    static class ModIni
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        static extern int GetPrivateProfileString(string section, string key, string def, StringBuilder ret, int size, string file);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool WritePrivateProfileString(string section, string key, string value, string file);

        public static string Read(string file, string section, string key, string def)
        {
            StringBuilder sb = new StringBuilder(64);
            GetPrivateProfileString(section, key, def, sb, sb.Capacity, file);
            return sb.ToString();
        }

        public static bool Write(string file, string section, string key, string value)
        {
            try { return WritePrivateProfileString(section, key, value, file); }
            catch { return false; }
        }

        public static readonly string[] LearnedKeys = { "LearnedTakeCamcorder", "LearnedRaise", "LearnedEject", "LearnedInsert", "LearnedDoor", "LearnedClimb", "LearnedCoverMouth" };
    }

    sealed partial class Viewfinder
    {
        // ---------------- pages ----------------
        public const int PageMain = 0, PageSettings = 1, PageControls = 2;
        public const int HitBack = 13, HitRow0 = 100, HitCallout0 = 200;
        static readonly Rectangle BackR = new Rectangle(1064, 96, 156, 34);
        static readonly Rectangle LayoutR = new Rectangle(800, 96, 244, 34);
        const int HitLayout = 14;
        bool indexControls;

        int page;
        double pageAt = -10;
        public int Page { get { return page; } }

        void OpenPage(int p)
        {
            if (p == page) return;
            page = p; pageAt = T;
            hover = HitNone;
            if (p == PageSettings) LoadSettings();
            if (p == PageControls) { calloutPick = -1; }
            if (sfx != null) sfx.Confirm();
        }

        void ClosePage()
        {
            if (page == PageMain) return;
            page = PageMain; pageAt = T;
            if (sfx != null) sfx.Tick();
        }

        // ---------------- the settings ----------------
        static readonly SettingDef[] Defs = BuildDefs();

        static SettingDef T_(string group, string section, string key, string label, int def, string help)
        {
            SettingDef d = new SettingDef(); d.Group = group; d.Section = section; d.Key = key; d.Label = label;
            d.Kind = SetKind.Toggle; d.Def = def; d.Help = help; return d;
        }
        static SettingDef C_(string group, string section, string key, string label, int def, string[] names, double[] values, string help)
        {
            SettingDef d = new SettingDef(); d.Group = group; d.Section = section; d.Key = key; d.Label = label;
            d.Kind = SetKind.Choice; d.Def = def; d.Names = names; d.Values = values; d.Help = help; return d;
        }
        static SettingDef N_(string group, string section, string key, string label, double def, double min, double max, double step, string unit, string off, string help)
        {
            SettingDef d = new SettingDef(); d.Group = group; d.Section = section; d.Key = key; d.Label = label;
            d.Kind = SetKind.Number; d.Def = def; d.Min = min; d.Max = max; d.Step = step; d.Unit = unit; d.Off = off; d.Help = help; return d;
        }
        static SettingDef A_(string group, string label, int action, string help)
        {
            SettingDef d = new SettingDef(); d.Group = group; d.Label = label; d.Kind = SetKind.Action; d.Action = action; d.Help = help; return d;
        }

        static SettingDef[] BuildDefs()
        {
            return new SettingDef[]
            {
                C_("COMFORT", "Input", "TurnMode", "Turning", 0, new[] { "SMOOTH", "SNAP" }, new double[] { 0, 1 },
                   "SMOOTH turns while you hold the right stick.\nSNAP jumps a fixed angle per flick - easier on\nthe stomach for most people."),
                C_("COMFORT", "Input", "SnapTurnDeg", "Snap angle", 30, new[] { "15°", "30°", "45°", "60°", "90°" }, new double[] { 15, 30, 45, 60, 90 },
                   "How far one flick of the right stick turns you\nwhen Turning is SNAP."),
                C_("COMFORT", "Input", "SmoothTurnDegPerSec", "Smooth turn speed", 260, new[] { "60°/s", "90°/s", "120°/s", "150°/s", "180°/s", "220°/s", "MAX" },
                   new double[] { 60, 90, 120, 150, 180, 220, 260 },
                   "How fast SMOOTH turning spins you at full stick,\nin degrees a second. Lower is gentler."),
                T_("COMFORT", "Input", "SnapTurnBlink", "Blink while snapping", 1,
                   "The view darkens for the instant a snap turn\ntakes, so your eyes never see the jump."),
                N_("COMFORT", "Render", "WorldSize", "World size", 1.0, 0.6, 1.5, 0.05, "%", null,
                   "100% = true size, your own eye spacing.\nThings look too big or zoomed in? Lower it.\nToo small? Raise it."),
                N_("COMFORT", "Comfort", "ReduceHeadBob", "Reduce head bob", 1.0, 0.0, 1.0, 0.1, "%", "OFF",
                   "100% = no head bob walking or running: no\nbounce, rocking or sway. OFF = the game's own.\nStairs, jumps and falls still move you."),
                T_("COMFORT", "Comfort", "LevelHorizon", "Keep the horizon level", 1,
                   "Removes the game's own camera tilt during play.\nYour head still tilts freely."),
                C_("MOVEMENT", "Input", "WalkDirection", "Walk direction", 0, new[] { "BODY", "HEAD", "LEFT HAND" }, new double[] { 0, 1, 2 },
                   "Which way pushing the left stick forward walks:\nwhere Miles faces, where you look, or where your\nleft controller points."),
                T_("MOVEMENT", "Input", "StickPitch", "Right stick tilts the view", 0,
                   "OFF: your head does all the looking up and down,\nlike a native VR game. ON: the old gamepad\nbehaviour."),
                T_("MOVEMENT", "Motion", "BodyFollowsHead", "Body turns with you", 1,
                   "Turn your body or chair and Miles turns to face\nthe same way after a moment - arms and holster\nline up again without a recenter."),
                T_("MOVEMENT", "Motion", "RoomscaleWalk", "Walk around for real", 1,
                   "Step around your room and Miles's body walks\nwith you. Walls and shut doors stop him like the\nstick does. OFF: the body stays, your head leans."),
                T_("MOVEMENT", "Input", "PhysicalCrouch", "Crouch when you duck", 0,
                   "Duck for real and Miles crouches; stand up and he\nstands. B still crouches too."),
                N_("MOVEMENT", "Input", "CrouchDepthM", "Duck depth", 0.30, 0.15, 0.60, 0.05, "cm", null,
                   "How far below your standing height your head\nhas to go before Miles crouches."),
                C_("HANDS", "Motion", "CamcorderControl", "Camcorder controls", 0, new[] { "HANDS-ON", "GRIP BUTTON" }, new double[] { 0, 1 },
                   "HANDS-ON: take it from your right hip, bring it\nto your eye. GRIP BUTTON: squeeze the right grip\nto raise it, again to lower - rest your hand."),
                T_("HANDS", "Motion", "CamcorderHolster", "Camcorder on your hip", 1,
                   "Reach to your right hip and squeeze the grip: the\ncamcorder glides into your hand. Bring it to your\neye to look through it. Let go: it goes back."),
                C_("HANDS", "Motion", "CamcorderHold", "Camcorder in your hand", 4, new[] { "FREE", "GAME POSE" }, new double[] { 4, 3 },
                   "FREE: it moves and turns exactly with your\ncontroller, like a real camcorder. GAME POSE: held\nthe way Miles holds it as he raises it."),
                N_("HANDS", "Motion", "RaiseToEyeReachM", "Camcorder to eye distance", 0.14, 0.08, 0.25, 0.01, "cm", null,
                   "How close the camcorder has to come to your eye\nbefore you are looking through it."),
                T_("HANDS", "Motion", "BatteryGesture", "Reload with your hands", 0,
                   "OFF: Y reloads the camcorder. ON: pull the old\nbattery out with your left hand, take a new one\nfrom your left hip and slide it in."),
                T_("HANDS", "Motion", "DoorGrab", "Hands-on doors", 1,
                   "Doors are heavy: push one with your open hand\nand it swings as far as you push, or squeeze the\ngrip on its handle and swing it. X still works."),
                C_("HANDS", "Motion", "ClimbMode", "Climbing", 2, new[] { "PHYSICAL", "GUIDED", "A BUTTON" }, new double[] { 2, 1, 0 },
                   "PHYSICAL: grab an edge or a rung and pull - your\nhands carry Miles, hand over hand. GUIDED: the\ngame's climb at your pace. A BUTTON: the old way."),
                T_("HANDS", "Motion", "AutoLedgeGrab", "Auto ledge grab", 0,
                   "ON: jump toward a ledge and Miles catches it\nhimself, like the original game. Pull down with\nboth grips or press A to climb up, B to drop."),
                T_("HANDS", "Render", "CamcorderBatteryGauge", "Large battery gauge", 1,
                   "While the camcorder is up, a big battery\ngauge sits low in your view, with the number\nof spare batteries beside it."),
                C_("HANDS", "Render", "HudScalePct", "Camcorder display size", 55, new[] { "40%", "50%", "55%", "60%", "63%", "70%", "80%", "100%" },
                   new double[] { 40, 50, 55, 60, 63, 70, 80, 100 },
                   "The battery meter, record light and timecode\nwhen the camcorder is up. Can't see them? Make\nthis smaller - it pulls them in from the edges."),
                T_("HANDS", "Motion", "ViewfinderFrame", "Zoom with black bars", 0,
                   "OFF: when you zoom, the zoomed picture fills\nyour view. ON: it sits in a camcorder-shaped\nwindow with black around it."),
                T_("HANDS", "Motion", "CamcorderViewfinder", "Zoom in a viewfinder", 1,
                   "While you zoom, the zoomed picture sits in a\ncamcorder viewfinder in front of your eyes."),
                T_("HANDS", "Motion", "HowToHints", "How-to labels", 1,
                   "Labels in the headset show what to do next.\nEach one stops after you have done it 3 times."),
                T_("HANDS", "Motion", "QuestPrompts", "Hints name your buttons", 1,
                   "The game's hints name your active controller\nbuttons, including Valve Index, instead of Xbox\nbuttons."),
                T_("HANDS", "Input", "Haptics", "Controller vibration", 1,
                   "The game's rumble in both controllers, plus the\nclicks and buzzes of the hands-on controls."),
                T_("HANDS", "Motion", "DevMarkers", "Developer markers", 0,
                   "Coloured rings for every grab zone (door handles,\nhip pouch, hip holster). For testing."),
                N_("PICTURE", "Render", "RenderScale", "Render resolution", 1.0, 0.6, 1.0, 0.05, "%", null,
                   "100% renders at the headset's own resolution.\nLower it if Outlast stutters in the headset."),
                N_("PICTURE", "XR", "Sharpness", "Sharpening", 0.5, 0.0, 1.0, 0.1, "%", "OFF",
                   "How much the picture is sharpened on its way to\nthe headset."),
                T_("PICTURE", "Render", "LensFit", "Lens-shaped eyes", 1,
                   "Each eye is drawn in the Quest's own lens shape:\nsharper, same cost. Off by itself on Quest Link.\nTurn off if the picture looks off-centre."),
                T_("PICTURE", "Render", "FilmGrain", "Film grain", 0,
                   "Outlast's film grain. OFF: a clean picture - in a\nheadset the grain reads as blur. ON: the game's\nown found-footage look."),
                C_("PICTURE", "Render", "DesktopMirrorFps", "Monitor picture", 30, new[] { "15 FPS", "30 FPS", "60 FPS", "EVERY FRAME" }, new double[] { 15, 30, 60, 0 },
                   "What your monitor shows while you play - the\nheadset always gets every frame. EVERY FRAME on\nthe monitor too makes the game a lot slower."),
                T_("PICTURE", "XR", "D3D9Ex", "Fast frame hand-over", 1,
                   "Hands every frame to the headset on the graphics\ncard (D3D9Ex). Turn off only if textures look wrong\nor Outlast crashes."),
                A_("TOOLS", "Show the how-to labels again", 1,
                   "Every how-to label in the headset shows again\nuntil you have done that action 3 more times."),
                A_("TOOLS", "Open the log folder", 2,
                   "%LOCALAPPDATA%\\OutlastVR - the log to send with\na bug report is OutlastVR_Log.txt."),
            };
        }

        double[] setVals = new double[0];
        int setSel, setTop;
        double setSelTime = -10, setChangeAt = -10;
        string iniPath, setStatus; Tone setStatusKind; double setStatusAt = -10;
        bool iniOk;
        const int SetRowH = 29, SetListY = 168, SetListH = 470, SetListX = 56, SetListW = 648;

        void LoadSettings()
        {
            iniPath = Path.Combine(gameDir, "outlastvr.ini");
            iniOk = File.Exists(iniPath);
            setVals = new double[Defs.Length];
            for (int i = 0; i < Defs.Length; i++)
            {
                SettingDef d = Defs[i];
                if (d.Kind == SetKind.Action) continue;
                double v = d.Def;
                if (iniOk)
                {
                    string s = ModIni.Read(iniPath, d.Section, d.Key, "");
                    double p;
                    if (s.Length > 0 && double.TryParse(s.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out p)) v = p;
                }
                setVals[i] = v;
            }
            if (!iniOk) SetStatus("outlastvr.ini NOT FOUND NEXT TO THE LAUNCHER", Tone.Err);
            else if (SysInfo.GameRunning()) SetStatus("OUTLAST IS RUNNING - CHANGES APPLY NEXT START", Tone.Warn);
            else SetStatus("CHANGES ARE SAVED THE MOMENT YOU MAKE THEM", Tone.Info);
        }

        void SetStatus(string s, Tone k) { setStatus = s; setStatusKind = k; setStatusAt = T; }

        // --settings-test DIR: the real load / change / save path against DIR\outlastvr.ini (a copy).
        bool settingsWriteTest;
        public string SettingsTest()
        {
            settingsWriteTest = true;
            StringBuilder r = new StringBuilder();
            page = PageSettings; LoadSettings();
            r.AppendLine("ini found: " + iniOk + " (" + iniPath + ")");
            for (int i = 0; i < Defs.Length; i++) if (Defs[i].Kind != SetKind.Action) r.AppendLine("  read " + Defs[i].Section + "." + Defs[i].Key + " = " + ValueText(i));
            int[] pick = { 0, 1, 2, 4, 6, 10, 11, 19 };
            foreach (int i in pick)
            {
                string before = ValueText(i);
                ChangeSetting(i, 1);
                r.AppendLine("  change " + Defs[i].Key + ": " + before + " -> " + ValueText(i) + "  [" + setStatus + "]  file=" + ModIni.Read(iniPath, Defs[i].Section, Defs[i].Key, "<missing>"));
            }
            return r.ToString();
        }

        int ChoiceIndex(SettingDef d, double v)
        {
            int best = 0; double bd = double.MaxValue;
            for (int i = 0; i < d.Values.Length; i++) { double e = Math.Abs(d.Values[i] - v); if (e < bd) { bd = e; best = i; } }
            return best;
        }

        string ValueText(int i)
        {
            SettingDef d = Defs[i];
            if (d.Kind == SetKind.Action) return "PRESS";
            double v = i < setVals.Length ? setVals[i] : d.Def;
            switch (d.Kind)
            {
                case SetKind.Toggle: return v != 0 ? "ON" : "OFF";
                case SetKind.Choice: return d.Names[ChoiceIndex(d, v)];
                default:
                    if (d.Off != null && Math.Abs(v) < 1e-6) return d.Off;
                    if (d.Unit == "%") return ((int)Math.Round(v * 100)).ToString(CultureInfo.InvariantCulture) + "%";
                    if (d.Unit == "cm") return ((int)Math.Round(v * 100)).ToString(CultureInfo.InvariantCulture) + " cm";
                    return v.ToString("0.00", CultureInfo.InvariantCulture);
            }
        }

        void ChangeSetting(int i, int dir)
        {
            if (i < 0 || i >= Defs.Length) return;
            SettingDef d = Defs[i];
            if (d.Kind == SetKind.Action) { if (dir != 0) RunSettingAction(d); return; }
            if (!iniOk) { SetStatus("outlastvr.ini NOT FOUND NEXT TO THE LAUNCHER", Tone.Err); if (sfx != null) sfx.Error(); return; }
            double v = setVals[i];
            string text;
            switch (d.Kind)
            {
                case SetKind.Toggle:
                    v = v != 0 ? 0 : 1; text = v != 0 ? "1" : "0"; break;
                case SetKind.Choice:
                {
                    int n = d.Values.Length, k = ChoiceIndex(d, v);
                    k = ((k + (dir == 0 ? 1 : dir)) % n + n) % n;
                    v = d.Values[k]; text = ((int)v).ToString(CultureInfo.InvariantCulture); break;
                }
                default:
                {
                    int steps = (int)Math.Round((v - d.Min) / d.Step);
                    steps += dir == 0 ? 1 : dir;
                    int maxSteps = (int)Math.Round((d.Max - d.Min) / d.Step);
                    if (dir == 0 && steps > maxSteps) steps = 0;               // ENTER wraps round
                    if (steps < 0) steps = 0; if (steps > maxSteps) steps = maxSteps;
                    v = Math.Round(d.Min + steps * d.Step, 4);
                    text = v.ToString("0.000", CultureInfo.InvariantCulture); break;
                }
            }
            if (Math.Abs(v - setVals[i]) < 1e-9) { if (sfx != null) sfx.Tick(); return; }
            if ((!preview || settingsWriteTest) && !ModIni.Write(iniPath, d.Section, d.Key, text))
            {
                SetStatus("COULD NOT WRITE outlastvr.ini", Tone.Err); if (sfx != null) sfx.Error(); return;
            }
            setVals[i] = v; setChangeAt = T;
            SetStatus("SAVED: " + d.Label.ToUpperInvariant() + " = " + ValueText(i), Tone.Ok);
            if (sfx != null) sfx.Confirm();
        }

        void RunSettingAction(SettingDef d)
        {
            if (d.Action == 1)
            {
                if (!iniOk) { SetStatus("outlastvr.ini NOT FOUND NEXT TO THE LAUNCHER", Tone.Err); if (sfx != null) sfx.Error(); return; }
                if (!preview) foreach (string k in ModIni.LearnedKeys) ModIni.Write(iniPath, "Motion", k, "0");
                SetStatus("HOW-TO LABELS WILL SHOW AGAIN", Tone.Ok);
                if (sfx != null) sfx.Success();
            }
            else if (d.Action == 2)
            {
                string dir = SysInfo.LogDir();
                if (!Directory.Exists(dir)) { SetStatus("NO LOG FOLDER YET - PLAY IN VR ONCE", Tone.Warn); if (sfx != null) sfx.Error(); return; }
                if (!preview)
                {
                    try
                    {
                        ProcessStartInfo psi = new ProcessStartInfo("explorer.exe", "\"" + dir + "\"");
                        psi.UseShellExecute = true;
                        Process.Start(psi);
                    }
                    catch (Exception ex) { SetStatus("COULD NOT OPEN FOLDER: " + ex.Message, Tone.Err); return; }
                }
                SetStatus("OPENED %LOCALAPPDATA%\\OutlastVR", Tone.Ok);
                if (sfx != null) sfx.Confirm();
            }
            setChangeAt = T;
        }

        // The list as drawn: a header line before each group's first setting.
        struct SetLine { public int Def; public string Header; }
        List<SetLine> SetLines()
        {
            List<SetLine> l = new List<SetLine>();
            string g = null;
            for (int i = 0; i < Defs.Length; i++)
            {
                if (Defs[i].Group != g) { g = Defs[i].Group; SetLine h = new SetLine(); h.Def = -1; h.Header = g; l.Add(h); }
                SetLine s = new SetLine(); s.Def = i; l.Add(s);
            }
            return l;
        }

        int VisibleLines { get { return SetListH / SetRowH; } }

        void SettingsSelect(int i)
        {
            if (i < 0) i = 0; if (i >= Defs.Length) i = Defs.Length - 1;
            if (i == setSel) return;
            setSel = i; setSelTime = T;
            if (sfx != null) sfx.Tick();
            EnsureSelVisible();
        }

        void EnsureSelVisible()
        {
            List<SetLine> lines = SetLines();
            int at = 0;
            for (int k = 0; k < lines.Count; k++) if (lines[k].Def == setSel) { at = k; break; }
            if (at > 0 && lines[at - 1].Def < 0 && at - 1 < setTop) setTop = at - 1;   // keep its group header in view
            if (at < setTop) setTop = at;
            int vis = VisibleLines;
            if (at >= setTop + vis) setTop = at - vis + 1;
            if (setTop < 0) setTop = 0;
        }

        public void Wheel(int delta)
        {
            if (page == PageSettings)
            {
                int max = Math.Max(0, SetLines().Count - VisibleLines);
                setTop -= Math.Sign(delta) * 2;
                if (setTop < 0) setTop = 0; if (setTop > max) setTop = max;
            }
        }

        // ---------------- the controls tape ----------------
        // Callouts per controller, top to bottom: trigger, upper face button, thumbstick, lower face button,
        // Menu (left) / Meta (right), grip.
        static readonly string[,] CallTitle =
        {
            { "LEAN LEFT", "RELOAD (Y)", "MOVE", "USE / OPEN (X)", "PAUSE (MENU)", "SPRINT / GRAB" },
            { "LEAN RIGHT", "CROUCH (B)", "TURN", "JUMP (A)", "QUEST MENU (META)", "TAKE CAMCORDER" },
        };
        static readonly string[,] CallSub =
        {
            { "trigger · look back while running", "+ left grip: documents", "click the stick: sprint", "+ left grip: notes", "+ left grip: VR settings menu", "hold to sprint · handles, ledges" },
            { "trigger · look back while running", "+ left grip: notebook", "camcorder up: zoom · click: night vision", "climb, vault", "the headset's own menu", "from your hip · handles, ledges" },
        };
        int calloutPick = -1; double calloutPickAt = -10;

        int CurrentCallout(double t)
        {
            if (calloutPick >= 0 && t - calloutPickAt < 5) return calloutPick;
            return (int)(Math.Max(0, t - pageAt) / 1.7) % 12;
        }

        // Callout label boxes (left column for the left controller, right column for the right).
        static RectangleF CalloutBox(int side, int k)
        {
            float y = 168 + k * 62;
            return side == 0 ? new RectangleF(54, y, 290, 52) : new RectangleF(936, y, 290, 52);
        }

        // ---------------- input on the pages ----------------
        int PageHitTest(Point p)
        {
            lastMouse = p;
            if (BackR.Contains(p)) return HitBack;
            if (page == PageControls && LayoutR.Contains(p)) return HitLayout;
            if (page == PageSettings)
            {
                List<SetLine> lines = SetLines();
                int vis = VisibleLines;
                for (int k = 0; k < vis && setTop + k < lines.Count; k++)
                {
                    SetLine l = lines[setTop + k];
                    if (l.Def < 0) continue;
                    Rectangle r = new Rectangle(SetListX, SetListY + k * SetRowH, SetListW, SetRowH - 3);
                    if (r.Contains(p)) return HitRow0 + l.Def;
                }
            }
            else if (page == PageControls)
            {
                for (int side = 0; side < 2; side++)
                    for (int k = 0; k < 6; k++)
                        if (CalloutBox(side, k).Contains(p)) return HitCallout0 + side * 6 + k;
            }
            return HitNone;
        }

        Point lastMouse;
        void PageClick(int hit)
        {
            if (hit == HitBack) { ClosePage(); return; }
            if (hit == HitLayout) { indexControls = !indexControls; return; }
            if (hit >= HitRow0 && hit < HitRow0 + Defs.Length)
            {
                int i = hit - HitRow0;
                SettingsSelect(i);
                // the left arrow of the value lowers it, anywhere else raises / toggles it
                int dir = lastMouse.X < SetListX + SetListW - 168 + 24 && lastMouse.X > SetListX + SetListW - 190 ? -1 : 1;
                ChangeSetting(i, dir);
                return;
            }
            if (hit >= HitCallout0 && hit < HitCallout0 + 12) { calloutPick = hit - HitCallout0; calloutPickAt = T; if (sfx != null) sfx.Tick(); }
        }

        bool PageKey(Keys k, bool shift)
        {
            if (k == Keys.Escape || k == Keys.Back) { ClosePage(); return true; }
            if (page == PageSettings)
            {
                switch (k)
                {
                    case Keys.Up: case Keys.W: SettingsSelect(setSel - 1); return true;
                    case Keys.Down: case Keys.S: SettingsSelect(setSel + 1); return true;
                    case Keys.Tab: SettingsSelect(setSel + (shift ? -1 : 1)); return true;
                    case Keys.Home: SettingsSelect(0); return true;
                    case Keys.End: SettingsSelect(Defs.Length - 1); return true;
                    case Keys.PageUp: SettingsSelect(setSel - 6); return true;
                    case Keys.PageDown: SettingsSelect(setSel + 6); return true;
                    case Keys.Left: case Keys.A: ChangeSetting(setSel, -1); return true;
                    case Keys.Right: case Keys.D: ChangeSetting(setSel, 1); return true;
                    case Keys.Enter: case Keys.Space: ChangeSetting(setSel, 0); return true;
                }
            }
            else if (page == PageControls)
            {
                if (k == Keys.I) { indexControls = !indexControls; return true; }
                int cur = CurrentCallout(T);
                switch (k)
                {
                    case Keys.Up: case Keys.W: case Keys.Left: case Keys.A:
                        calloutPick = (cur + 11) % 12; calloutPickAt = T; if (sfx != null) sfx.Tick(); return true;
                    case Keys.Down: case Keys.S: case Keys.Right: case Keys.D: case Keys.Tab:
                        calloutPick = (cur + 1) % 12; calloutPickAt = T; if (sfx != null) sfx.Tick(); return true;
                    case Keys.Enter: case Keys.Space: ClosePage(); return true;
                }
            }
            return false;
        }

        public void MoveH(int d)
        {
            if (page == PageSettings) ChangeSetting(setSel, d);
            else if (page == PageControls) PageKey(d < 0 ? Keys.Left : Keys.Right, false);
        }

        public void PreviewPage(int p, int sel, double since)
        {
            if (p == 3) { p = PageControls; indexControls = true; }
            page = p; pageAt = previewT - since;
            if (p == PageSettings) { LoadSettings(); setSel = Math.Max(0, Math.Min(Defs.Length - 1, sel)); setSelTime = previewT - 1.5; EnsureSelVisible(); }
            if (p == PageControls && sel >= 0) { calloutPick = sel % 12; calloutPickAt = previewT - 0.6; }
        }

        // ---------------- drawing the pages ----------------
        void DrawPage(Graphics g, double t, double ua)
        {
            double pa = ua * U.Smooth(0.05, 0.40, t - pageAt);
            // the corridor stays behind, darker, so the page reads
            Fill(g, U.A(Color.FromArgb(150, 0, 6, 2), ua), 0, 0, W, H);
            string tape = page == PageSettings ? "TAPE 02 — VR SETTINGS" : indexControls ? "TAPE 03 — VALVE INDEX CONTROLLERS" : "TAPE 03 — QUEST 3 TOUCH PLUS CONTROLLERS";
            string head = page == PageSettings ? "VR SETTINGS" : indexControls ? "VALVE INDEX CONTROLS" : "QUEST 3 CONTROLS";
            double ts = Math.Max(0, t - pageAt - 0.15);
            int n = Math.Min(tape.Length, (int)(ts * 40));
            Text(g, head, fBtnBig, U.A(Hi, pa), 58, 92, true);
            Text(g, tape.Substring(0, n) + (n < tape.Length || ((int)(t * 2.2)) % 2 == 0 ? "█" : ""), fLabel, U.A(NvMain, pa), 60, 132, true);
            Fill(g, U.A(Color.FromArgb(70, 120, 220, 130), pa), 60, 156, page == PageSettings ? SetListW : 1166, 1);
            DrawBack(g, t, pa);
            if (page == PageControls) {
                Fill(g, U.A(Color.FromArgb(150, 12, 45, 20), pa), LayoutR);
                Text(g, indexControls ? "I: SHOW QUEST" : "I: SHOW INDEX", fStatB, U.A(Hi, pa), LayoutR.X + 15, LayoutR.Y + 6, true);
            }
            if (page == PageSettings) DrawSettings(g, t, pa);
            else DrawControls(g, t, pa);
            string hint = page == PageSettings ? "↑↓ SELECT    ◄► CHANGE    ENTER TOGGLE    ESC / B BACK    F11 FULL SCREEN" : "◄► STEP THROUGH THE CONTROLS    ESC / B BACK    F11 FULL SCREEN";
            TextCenter(g, hint, fHint, U.A(Color.FromArgb(150, 160, 215, 168), pa), 640, 690, true);
        }

        void DrawBack(Graphics g, double t, double a)
        {
            Rectangle r = BackR;
            bool hot = hover == HitBack;
            Fill(g, U.A(hot ? Color.FromArgb(150, 36, 128, 54) : Color.FromArgb(135, 1, 8, 3), a), r);
            pen.Width = hot ? 1.6f : 1f; pen.Color = U.A(hot ? Color.FromArgb(240, 175, 255, 185) : Color.FromArgb(110, 80, 140, 90), a);
            g.DrawRectangle(pen, r);
            sb.Color = U.A(hot ? Hi : NvMain, a);
            float cx = r.X + 22, cy = r.Y + r.Height / 2f;
            g.FillPolygon(sb, new[] { new PointF(cx - 8, cy), new PointF(cx + 2, cy - 8), new PointF(cx + 2, cy + 8) });
            Text(g, "BACK", fBtn, U.A(hot ? Hi : Color.FromArgb(225, 150, 214, 156), a), r.X + 40, r.Y + 5, true);
        }

        void DrawSettings(Graphics g, double t, double a)
        {
            List<SetLine> lines = SetLines();
            int vis = VisibleLines;
            for (int k = 0; k < vis && setTop + k < lines.Count; k++)
            {
                SetLine l = lines[setTop + k];
                int y = SetListY + k * SetRowH;
                double appear = U.Smooth(0.10 + k * 0.025, 0.35 + k * 0.025, t - pageAt);
                double ra = a * appear;
                if (ra < 0.01) continue;
                int dx = (int)((1 - appear) * -24);
                if (l.Def < 0)
                {
                    Text(g, l.Header, fStatB, U.A(NvDim, ra), SetListX + 4 + dx, y + 8, false);
                    float w = Measure(g, l.Header, fStatB);
                    Fill(g, U.A(Color.FromArgb(80, 110, 200, 120), ra), SetListX + 14 + (int)w + dx, y + 16, SetListW - 18 - (int)w, 1);
                    continue;
                }
                DrawSettingRow(g, l.Def, new Rectangle(SetListX + dx, y, SetListW, SetRowH - 3), t, ra);
            }
            // scroll marker
            int total = lines.Count;
            if (total > vis)
            {
                int trackY = SetListY, trackH = vis * SetRowH - 3;
                int thumbH = Math.Max(24, trackH * vis / total), thumbY = trackY + (trackH - thumbH) * setTop / Math.Max(1, total - vis);
                Fill(g, U.A(Color.FromArgb(60, 120, 220, 130), a), SetListX + SetListW + 8, trackY, 2, trackH);
                Fill(g, U.A(Color.FromArgb(200, 160, 255, 170), a), SetListX + SetListW + 7, thumbY, 4, thumbH);
            }
            DrawSettingHelp(g, t, a);
        }

        void DrawSettingRow(Graphics g, int i, Rectangle r, double t, double a)
        {
            SettingDef d = Defs[i];
            bool selected = i == setSel;
            bool hot = hover == HitRow0 + i;
            bool blink = ((int)(t * 4)) % 2 == 0;
            if (selected)
            {
                using (LinearGradientBrush lb = new LinearGradientBrush(new Rectangle(r.X - 1, r.Y, r.Width + 2, r.Height), U.A(Color.FromArgb(150, 36, 128, 54), a), U.A(Color.FromArgb(60, 6, 36, 14), a), 0f))
                    g.FillRectangle(lb, r);
                pen.Width = 1.4f; pen.Color = U.A(Color.FromArgb(240, 175, 255, 185), a); g.DrawRectangle(pen, r);
                Fill(g, U.A(Color.FromArgb(250, 160, 255, 170), a), r.X, r.Y, 4, r.Height);
                // the sweep that crosses a selected camcorder button
                double swp = (t * 0.65) % 1.0; int sx = r.X + (int)(swp * (r.Width + 120)) - 60;
                Rectangle sr = new Rectangle(sx - 50, r.Y + 1, 100, r.Height - 1);
                using (LinearGradientBrush lb = new LinearGradientBrush(new Rectangle(sr.X - 1, sr.Y, sr.Width + 2, sr.Height), Color.Transparent, Color.Transparent, 0f))
                {
                    ColorBlend cb = new ColorBlend(3);
                    cb.Colors = new[] { Color.FromArgb(0, 210, 255, 215), U.A(Color.FromArgb(30, 210, 255, 215), a), Color.FromArgb(0, 210, 255, 215) };
                    cb.Positions = new[] { 0f, 0.5f, 1f };
                    lb.InterpolationColors = cb;
                    Region old = g.Clip; g.SetClip(r); g.FillRectangle(lb, sr); g.Clip = old;
                }
            }
            else Fill(g, U.A(Color.FromArgb(hot ? 150 : 110, 1, 10, 4), a), r);
            Color lc = selected ? Hi : (d.Kind == SetKind.Action ? Color.FromArgb(225, 170, 220, 175) : Color.FromArgb(225, 150, 214, 156));
            Text(g, d.Label, fStatB, U.A(lc, a), r.X + 16, r.Y + 6, true);
            // value: ◄ VALUE ► when selected
            string v = ValueText(i);
            Color vc;
            if (d.Kind == SetKind.Action) vc = selected ? Amber : Color.FromArgb(170, 200, 170, 90);
            else if (d.Kind == SetKind.Toggle) vc = setVals.Length > i && setVals[i] != 0 ? (selected ? Hi : NvMain) : Color.FromArgb(200, 150, 160, 150);
            else vc = selected ? Hi : NvMain;
            float vx = r.Right - 100;
            bool justChanged = selected && t - setChangeAt < 0.32;
            if (justChanged) GlitchText(g, v, fStatB, U.A(vc, a), vx - Measure(g, v, fStatB) / 2, r.Y + 6, 1 - (t - setChangeAt) / 0.32, i);
            else TextCenter(g, v, fStatB, U.A(vc, a), vx, r.Y + 6, true);
            if (selected && d.Kind != SetKind.Action && (blink || t - setSelTime < 0.3))
            {
                sb.Color = U.A(Color.FromArgb(240, 180, 255, 190), a);
                float cy = r.Y + r.Height / 2f;
                g.FillPolygon(sb, new[] { new PointF(vx - 78, cy), new PointF(vx - 70, cy - 6), new PointF(vx - 70, cy + 6) });
                g.FillPolygon(sb, new[] { new PointF(vx + 78, cy), new PointF(vx + 70, cy - 6), new PointF(vx + 70, cy + 6) });
            }
            if (justChanged) Fill(g, U.A(Color.FromArgb((int)(90 * (1 - (t - setChangeAt) / 0.32)), 210, 255, 215), a), r);
        }

        void DrawSettingHelp(Graphics g, double t, double a)
        {
            Rectangle r = new Rectangle(740, 168, 484, 300);
            Fill(g, U.A(Color.FromArgb(172, 0, 7, 3), a), r);
            pen.Width = 1; pen.Color = U.A(Color.FromArgb(140, 90, 165, 100), a); g.DrawRectangle(pen, r);
            Fill(g, U.A(Color.FromArgb(70, 90, 210, 110), a), r.X + 1, r.Y + 1, r.Width - 1, 24);
            Text(g, "WHAT IT DOES", fStatB, U.A(Hi, a), r.X + 12, r.Y + 6, false);
            TextRight(g, "CAM 02", fStatB, U.A(NvMain, a), r.Right - 12, r.Y + 6, false);
            SettingDef d = Defs[Math.Max(0, Math.Min(Defs.Length - 1, setSel))];
            double age = t - setSelTime;
            Text(g, d.Label.ToUpperInvariant(), fBtn, U.A(Hi, a), r.X + 14, r.Y + 38, true);
            if (d.Kind != SetKind.Action)
            {
                string v = ValueText(setSel);
                Text(g, "NOW:", fStatB, U.A(NvDim, a), r.X + 16, r.Y + 74, false);
                Text(g, v, fBtn, U.A(NvMain, a), r.X + 64, r.Y + 68, true);
            }
            // the description types itself out like the tape log
            string[] help = d.Help.Split('\n');
            int chars = (int)(Math.Max(0, age - 0.05) * 140);
            int y = r.Y + 108;
            foreach (string line in help)
            {
                if (chars <= 0) break;
                int n = Math.Min(line.Length, chars); chars -= line.Length;
                Text(g, line.Substring(0, n) + (n < line.Length ? "█" : ""), fStat, U.A(Color.FromArgb(240, 214, 242, 216), a), r.X + 16, y, false);
                y += 20;
            }
            Text(g, "[" + d.Section + "] " + (d.Key ?? ""), fLog, U.A(NvDim, a), r.X + 16, r.Bottom - 26, false);
            if (d.Kind == SetKind.Action) Text(g, "ENTER / CLICK TO DO IT", fStatB, U.A(Amber, a), r.X + 250, r.Bottom - 26, false);
            // status line (saved, warnings)
            Rectangle s = new Rectangle(740, 480, 484, 64);
            Fill(g, U.A(Color.FromArgb(172, 0, 7, 3), a), s);
            pen.Width = 1; pen.Color = U.A(Color.FromArgb(120, 90, 165, 100), a); g.DrawRectangle(pen, s);
            Text(g, "TAPE LOG", fIdx, U.A(NvDim, a), s.X + 12, s.Y + 8, false);
            if (setStatus != null)
            {
                Color sc = setStatusKind == Tone.Ok ? NvMain : setStatusKind == Tone.Warn ? Amber : setStatusKind == Tone.Err ? ErrC : Color.FromArgb(220, 175, 215, 180);
                int n = Math.Min(setStatus.Length, (int)((t - setStatusAt) * 90) + 1);
                Text(g, "> " + setStatus.Substring(0, n) + (n < setStatus.Length ? "█" : ""), fLog, U.A(sc, a), s.X + 12, s.Y + 30, false);
            }
            string where = iniPath != null ? ShortPath(iniPath, 52) : "";
            Text(g, where, fLog, U.A(NvDim, a * 0.8), 742, 556, false);
        }

        // ---------------- the controllers ----------------
        void DrawIndexControls(Graphics g, double a)
        {
            string[,] labels = {
                { "LEFT HAND", "RIGHT HAND" },
                { "A: INTERACT / OPEN", "A: JUMP / VAULT" },
                { "B: RELOAD BATTERY", "B: CROUCH / DROP" },
                { "STICK: WALK / CLICK TO SPRINT", "STICK: TURN / UP-DOWN TO ZOOM" },
                { "TRIGGER: LEAN / LOOK BACK", "STICK CLICK: NIGHT VISION" },
                { "CLOSE HAND: SPRINT / GRAB", "CLOSE HAND: CAMCORDER / GRAB" },
                { "TRACKPAD PRESS: PAUSE", "TRIGGER: LEAN / LOOK BACK" }
            };
            for (int side = 0; side < 2; ++side)
                for (int row = 0; row < 7; ++row)
                    Text(g, labels[row, side], row == 0 ? fBtnBig : fStatB,
                         U.A(row == 0 ? Hi : NvMain, a), 70 + side * 590, 178 + row * 48, true);
            Text(g, "LEFT GRIP + LEFT TRACKPAD: VR SETTINGS     BOTH STICK CLICKS: RECENTER", fStatB, U.A(Hi, a), 70, 538, true);
            Text(g, "Open your hand to release. Left grip + left A: recordings; + left B: documents; + right B: notebook.", fHint, U.A(NvMain, a), 70, 585, false);
            Text(g, "Finger animation uses runtime hand tracking when available. System buttons open SteamVR.", fHint, U.A(NvMain, a), 70, 615, false);
        }

        void DrawControls(Graphics g, double t, double a)
        {
            if (indexControls) { DrawIndexControls(g, a); return; }
            double since = t - pageAt;
            int cur = CurrentCallout(t);
            PointF[][] pts = new PointF[2][];
            for (int side = 0; side < 2; side++)
            {
                double appear = U.Smooth(0.15 + side * 0.12, 0.65 + side * 0.12, since);
                float bob = (float)(Math.Sin(t * 1.15 + side * 1.9) * 5);
                float rot = (float)(Math.Sin(t * 0.7 + side * 2.4) * 2.2 + (side == 0 ? -7 : 7));
                PointF c = side == 0 ? new PointF(480, 262 + bob) : new PointF(800, 262 - bob);
                c.Y += (float)((1 - appear) * 40);
                int hl = cur / 6 == side ? cur % 6 : -1;
                pts[side] = DrawTouchPlus(g, c, side == 1, rot, t, a * appear, hl);
            }
            // callouts: title, sub line, a leader to the control
            for (int side = 0; side < 2; side++)
            {
                for (int k = 0; k < 6; k++)
                {
                    int idx = side * 6 + k;
                    double appear = U.Smooth(0.55 + idx * 0.05, 0.85 + idx * 0.05, since);
                    double la = a * appear; if (la < 0.01) continue;
                    bool on = idx == cur;
                    bool hot = hover == HitCallout0 + idx;
                    RectangleF box = CalloutBox(side, k);
                    if (on || hot)
                    {
                        Fill(g, U.A(Color.FromArgb(on ? 120 : 70, 30, 110, 46), la), Rectangle.Round(box));
                        pen.Width = 1.3f; pen.Color = U.A(Color.FromArgb(220, 160, 255, 175), la); g.DrawRectangle(pen, Rectangle.Round(box));
                    }
                    else Fill(g, U.A(Color.FromArgb(120, 1, 9, 4), la), Rectangle.Round(box));
                    string title = CallTitle[side, k], sub = CallSub[side, k];
                    double ontime = on ? t - Math.Max(pageAt, Math.Floor((t - pageAt) / 1.7) * 1.7 + pageAt) : 9;
                    if (calloutPick == idx && t - calloutPickAt < 5) ontime = t - calloutPickAt;
                    int tn = on ? Math.Min(title.Length, (int)(ontime * 60) + 1) : title.Length;
                    Color tc = on ? Hi : Color.FromArgb(220, 150, 214, 156);
                    float tx = side == 0 ? box.Right - 10 - Measure(g, title, fOsdMid) : box.X + 10;
                    Text(g, title.Substring(0, tn) + (on && tn < title.Length ? "█" : ""), fOsdMid, U.A(tc, la), tx, box.Y + 6, true);
                    float sx = side == 0 ? box.Right - 10 - Measure(g, sub, fSub) : box.X + 10;
                    Text(g, sub, fSub, U.A(on ? Color.FromArgb(235, 175, 245, 185) : Color.FromArgb(180, 120, 180, 128), la), sx, box.Y + 29, false);
                    // leader: from the box edge to the control, with an elbow and a dot
                    PointF p = pts[side][k];
                    PointF from = side == 0 ? new PointF(box.Right, box.Y + 18) : new PointF(box.X, box.Y + 18);
                    PointF elbow = new PointF(side == 0 ? from.X + 22 : from.X - 22, from.Y);
                    pen.Width = on ? 1.8f : 1f;
                    pen.Color = U.A(on ? Color.FromArgb(240, 190, 255, 200) : Color.FromArgb(110, 110, 190, 120), la);
                    if (on)
                    {
                        // the leader draws itself toward the control
                        double grow = U.Clamp01(ontime / 0.35);
                        PointF mid = new PointF((float)(elbow.X + (p.X - elbow.X) * grow), (float)(elbow.Y + (p.Y - elbow.Y) * grow));
                        g.DrawLine(pen, from, elbow); g.DrawLine(pen, elbow, mid);
                    }
                    else { g.DrawLine(pen, from, elbow); g.DrawLine(pen, elbow, p); }
                    sb.Color = pen.Color; g.FillEllipse(sb, p.X - 3, p.Y - 3, 6, 6);
                }
            }
            TextCenter(g, "BOTH STICK CLICKS: RECENTER", fStatB, U.A(Color.FromArgb(220, 150, 214, 156), a * U.Smooth(1.1, 1.5, since)), 640, 520, true);
            DrawGestures(g, t, a * U.Smooth(0.9, 1.3, since));
        }

        // One Touch Plus controller, seen from above and a little behind, in the camcorder's night vision.
        // Local units: the face is ~150 px across. Returns where each callout points (trigger, upper
        // button, stick, lower button, Menu/Meta, grip) in screen space.
        PointF[] DrawTouchPlus(Graphics g, PointF c, bool right, float rotDeg, double t, double a, int hl)
        {
            PointF[] outPts = new PointF[6];
            if (a < 0.01) return outPts;
            float m = right ? -1f : 1f;           // mirror: the right controller is the left one flipped
            GraphicsState gs = g.Save();
            g.TranslateTransform(c.X, c.Y);
            g.RotateTransform(rotDeg);
            using (Matrix mx = g.Transform.Clone())
            {
                // ---- handle (white plastic, lit green by night vision) ----
                using (GraphicsPath hp = new GraphicsPath())
                {
                    hp.AddBezier(-30 * m, 40, -46 * m, 110, -40 * m, 180, -18 * m, 236);
                    hp.AddBezier(-18 * m, 236, -8 * m, 252, 14 * m, 250, 20 * m, 232);
                    hp.AddBezier(20 * m, 232, 30 * m, 190, 40 * m, 120, 34 * m, 46);
                    hp.CloseFigure();
                    // glow
                    for (int k = 3; k >= 1; k--) { pen.Width = k * 5f; pen.Color = U.A(Color.FromArgb(18, 120, 255, 140), a); g.DrawPath(pen, hp); }
                    using (LinearGradientBrush lb = new LinearGradientBrush(new RectangleF(-50, 30, 100, 230), U.A(Color.FromArgb(235, 150, 200, 158), a), U.A(Color.FromArgb(235, 70, 112, 78), a), right ? 180f : 0f))
                        g.FillPath(lb, hp);
                    pen.Width = 1.6f; pen.Color = U.A(Color.FromArgb(230, 190, 250, 200), a); g.DrawPath(pen, hp);
                }
                // grip: the recessed pad on the inner side of the handle, where the middle finger rests
                PointF grip = new PointF(27 * m, 102);
                using (GraphicsPath gp = new GraphicsPath())
                {
                    gp.AddEllipse(grip.X - 9, grip.Y - 30, 18, 60);
                    bool on = hl == 5;
                    sb.Color = U.A(on ? Color.FromArgb(240, 200, 255, 210) : Color.FromArgb(210, 60, 96, 66), a); g.FillPath(sb, gp);
                    pen.Width = 1.2f; pen.Color = U.A(Color.FromArgb(220, 170, 240, 180), a); g.DrawPath(pen, gp);
                }
                // trigger: under the front of the face; shown as the curved paddle peeking out in front
                PointF trig = new PointF(-6 * m, -78);
                using (GraphicsPath tp = new GraphicsPath())
                {
                    tp.AddArc(-34, -92, 68, 34, 200, 140);
                    tp.AddArc(-26, -86, 52, 26, 340, -140);
                    tp.CloseFigure();
                    bool on = hl == 0;
                    sb.Color = U.A(on ? Color.FromArgb(240, 200, 255, 210) : Color.FromArgb(220, 120, 170, 128), a); g.FillPath(sb, tp);
                    pen.Width = 1.2f; pen.Color = U.A(Color.FromArgb(220, 190, 250, 200), a); g.DrawPath(pen, tp);
                }
                // ---- the round face ----
                RectangleF face = new RectangleF(-76, -68, 152, 136);
                for (int k = 3; k >= 1; k--) { pen.Width = k * 6f; pen.Color = U.A(Color.FromArgb(16, 120, 255, 140), a); g.DrawEllipse(pen, face); }
                using (GraphicsPath fp = new GraphicsPath())
                {
                    fp.AddEllipse(face);
                    using (PathGradientBrush pb = new PathGradientBrush(fp))
                    {
                        pb.CenterPoint = new PointF(-20 * m, -24);
                        pb.CenterColor = U.A(Color.FromArgb(255, 52, 74, 56), a);
                        pb.SurroundColors = new[] { U.A(Color.FromArgb(255, 18, 30, 21), a) };
                        g.FillPath(pb, fp);
                    }
                }
                pen.Width = 2f; pen.Color = U.A(Color.FromArgb(235, 150, 230, 160), a); g.DrawEllipse(pen, face);
                pen.Width = 1f; pen.Color = U.A(Color.FromArgb(90, 150, 230, 160), a); g.DrawEllipse(pen, -68, -60, 136, 120);
                // thumbstick: knurled cap with a dished top, upper outer side
                PointF stick = new PointF(-30 * m, -20);
                {
                    bool on = hl == 2;
                    sb.Color = U.A(Color.FromArgb(255, 10, 18, 12), a); g.FillEllipse(sb, stick.X - 25, stick.Y - 23, 50, 48);
                    sb.Color = U.A(on ? Color.FromArgb(255, 70, 140, 82) : Color.FromArgb(255, 36, 58, 40), a); g.FillEllipse(sb, stick.X - 21, stick.Y - 21, 42, 42);
                    pen.Width = 1.2f; pen.Color = U.A(Color.FromArgb(200, 160, 235, 170), a);
                    for (int k = 0; k < 24; k++)
                    {
                        double ang = k * Math.PI / 12;
                        g.DrawLine(pen, (float)(stick.X + Math.Cos(ang) * 17), (float)(stick.Y + Math.Sin(ang) * 17), (float)(stick.X + Math.Cos(ang) * 21), (float)(stick.Y + Math.Sin(ang) * 21));
                    }
                    sb.Color = U.A(on ? Color.FromArgb(255, 150, 230, 160) : Color.FromArgb(255, 62, 92, 66), a); g.FillEllipse(sb, stick.X - 12, stick.Y - 12, 24, 24);
                    pen.Color = U.A(Color.FromArgb(220, 190, 250, 200), a); g.DrawEllipse(pen, stick.X - 21, stick.Y - 21, 42, 42);
                }
                // the two face buttons (Y over X on the left, B over A on the right) and Menu / Meta
                PointF upper = new PointF(30 * m, -30), lower = new PointF(12 * m, 6), sys = new PointF(-34 * m, 26);
                DrawFaceButton(g, upper, 13, right ? "B" : "Y", hl == 1, a);
                DrawFaceButton(g, lower, 13, right ? "A" : "X", hl == 3, a);
                {
                    bool on = hl == 4;
                    sb.Color = U.A(on ? Color.FromArgb(255, 110, 200, 122) : Color.FromArgb(255, 30, 48, 34), a);
                    if (right) g.FillEllipse(sb, sys.X - 12, sys.Y - 8, 24, 16); else g.FillEllipse(sb, sys.X - 10, sys.Y - 10, 20, 20);
                    pen.Width = 1.2f; pen.Color = U.A(Color.FromArgb(220, 180, 245, 190), a);
                    if (right) g.DrawEllipse(pen, sys.X - 12, sys.Y - 8, 24, 16); else g.DrawEllipse(pen, sys.X - 10, sys.Y - 10, 20, 20);
                    pen.Width = 1.5f; pen.Color = U.A(on ? Color.FromArgb(255, 10, 30, 14) : Color.FromArgb(230, 190, 250, 200), a);
                    if (right) g.DrawLine(pen, sys.X - 4, sys.Y, sys.X + 4, sys.Y);                                            // the system button (plain)
                    else for (int k = -1; k <= 1; k++) g.DrawLine(pen, sys.X - 5, sys.Y + k * 4, sys.X + 5, sys.Y + k * 4);    // ☰ menu
                }
                // the highlight: rings spreading from the control being explained
                if (hl >= 0)
                {
                    PointF hp = hl == 0 ? trig : hl == 1 ? upper : hl == 2 ? stick : hl == 3 ? lower : hl == 4 ? sys : grip;
                    for (int k = 0; k < 2; k++)
                    {
                        double ph = ((t * 1.3) + k * 0.5) % 1.0;
                        float rr = (float)(14 + ph * 30);
                        pen.Width = 2f; pen.Color = U.A(Color.FromArgb((int)(220 * (1 - ph)), 200, 255, 210), a);
                        g.DrawEllipse(pen, hp.X - rr, hp.Y - rr, rr * 2, rr * 2);
                    }
                }
                PointF[] local = { trig, upper, stick, lower, sys, grip };
                mx.TransformPoints(local);
                outPts = local;
            }
            g.Restore(gs);
            // a soft green scan sweeping down the controller, like the camcorder focusing on it
            double sp = (t * 0.45 + (right ? 0.5 : 0)) % 1.0;
            float sy = c.Y - 90 + (float)(sp * 360);
            using (LinearGradientBrush lb = new LinearGradientBrush(new RectangleF(c.X - 110, sy - 14, 220, 28), Color.Transparent, Color.Transparent, 90f))
            {
                ColorBlend cb = new ColorBlend(3);
                cb.Colors = new[] { Color.FromArgb(0, 180, 255, 190), U.A(Color.FromArgb(26, 180, 255, 190), a), Color.FromArgb(0, 180, 255, 190) };
                cb.Positions = new[] { 0f, 0.5f, 1f };
                lb.InterpolationColors = cb;
                g.FillRectangle(lb, c.X - 110, sy - 14, 220, 28);
            }
            return outPts;
        }

        void DrawFaceButton(Graphics g, PointF p, float r, string glyph, bool on, double a)
        {
            sb.Color = U.A(Color.FromArgb(255, 8, 14, 10), a); g.FillEllipse(sb, p.X - r - 2, p.Y - r - 1, (r + 2) * 2, (r + 2) * 2);
            sb.Color = U.A(on ? Color.FromArgb(255, 150, 235, 160) : Color.FromArgb(255, 34, 54, 38), a); g.FillEllipse(sb, p.X - r, p.Y - r, r * 2, r * 2);
            pen.Width = 1.2f; pen.Color = U.A(Color.FromArgb(220, 180, 245, 190), a); g.DrawEllipse(pen, p.X - r, p.Y - r, r * 2, r * 2);
            GraphicsState s = g.Save();
            g.TranslateTransform(p.X, p.Y);
            float w = Measure(g, glyph, fStatB);
            sb.Color = U.A(on ? Color.FromArgb(255, 6, 26, 10) : Color.FromArgb(240, 200, 250, 205), a);
            g.DrawString(glyph, fStatB, sb, -w / 2, -9, sf);
            g.Restore(s);
        }

        // ---------------- the gestures strip ----------------
        // (Test40) the reload is a button again (Y, on the left controller); the third card is climbing
        static readonly string[] GestTitle = { "TAKE THE CAMCORDER", "LOOK THROUGH IT", "CLIMB", "OPEN A DOOR" };
        static readonly string[] GestLine1 = { "right hand to your hip,", "bring the camcorder", "both hands on the edge,", "push it with your hand," };
        static readonly string[] GestLine2 = { "squeeze the grip", "up to your eye", "squeeze and pull down", "or grab the handle" };

        void DrawGestures(Graphics g, double t, double a)
        {
            if (a < 0.01) return;
            int cur = (int)(Math.Max(0, t - pageAt) / 2.4) % 4;
            for (int i = 0; i < 4; i++)
            {
                Rectangle r = new Rectangle(56 + i * 294, 548, 282, 92);
                bool on = i == cur;
                Fill(g, U.A(on ? Color.FromArgb(150, 20, 80, 32) : Color.FromArgb(150, 1, 9, 4), a), r);
                pen.Width = on ? 1.5f : 1f; pen.Color = U.A(on ? Color.FromArgb(230, 170, 255, 185) : Color.FromArgb(110, 80, 140, 90), a); g.DrawRectangle(pen, r);
                Fill(g, U.A(on ? Color.FromArgb(250, 160, 255, 170) : Color.FromArgb(120, 70, 140, 80), a), r.X, r.Y, 4, r.Height);
                DrawGestureIcon(g, i, new RectangleF(r.X + 10, r.Y + 8, 76, 76), t, on, a);
                Text(g, GestTitle[i], fStatB, U.A(on ? Hi : Color.FromArgb(225, 150, 214, 156), a), r.X + 94, r.Y + 14, true);
                Text(g, GestLine1[i], fSub, U.A(Color.FromArgb(225, 190, 235, 195), a), r.X + 94, r.Y + 40, false);
                Text(g, GestLine2[i], fSub, U.A(Color.FromArgb(225, 190, 235, 195), a), r.X + 94, r.Y + 58, false);
            }
        }

        // Little animated pictograms in the OSD's line style.
        void DrawGestureIcon(Graphics g, int which, RectangleF r, double t, bool on, double a)
        {
            Color line = on ? Color.FromArgb(250, 200, 255, 210) : Color.FromArgb(200, 140, 210, 150);
            pen.Width = 2f; pen.Color = U.A(line, a);
            float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2;
            double ph = on ? (t * 0.8) % 1.0 : 0.6;
            double e = U.Smooth(0, 0.6, ph);
            switch (which)
            {
                case 0:   // a torso, the camcorder on the hip, the hand reaching down for it
                {
                    g.DrawEllipse(pen, cx - 9, r.Y + 2, 18, 18);
                    g.DrawLine(pen, cx - 22, r.Y + 26, cx + 22, r.Y + 26);
                    g.DrawLine(pen, cx - 22, r.Y + 26, cx - 18, r.Bottom - 4); g.DrawLine(pen, cx + 22, r.Y + 26, cx + 18, r.Bottom - 4);
                    RectangleF cam = new RectangleF(cx + 14, r.Y + 52, 16, 10);
                    sb.Color = U.A(line, a); g.FillRectangle(sb, cam);
                    float hx = (float)(r.Right - 2), hy = (float)(r.Y + 30 + e * 22);
                    g.DrawLine(pen, cx + 22, r.Y + 26, hx, hy);
                    g.DrawEllipse(pen, hx - 5, hy - 5, 10, 10);
                    break;
                }
                case 1:   // the camcorder rising to an eye
                {
                    g.DrawArc(pen, r.X + 4, r.Y + 6, 34, 22, 200, 140); g.DrawArc(pen, r.X + 4, r.Y + 6, 34, 22, 20, 140);
                    sb.Color = U.A(line, a); g.FillEllipse(sb, r.X + 16, r.Y + 12, 10, 10);
                    float camY = (float)(r.Bottom - 22 - e * 34);
                    g.DrawRectangle(pen, r.X + 34, camY, 30, 18);
                    g.DrawLine(pen, r.X + 34, camY + 9, r.X + 26, camY + 9);
                    g.DrawLine(pen, r.X + 49, r.Bottom - 2, r.X + 49, camY + 18);
                    break;
                }
                case 2:   // both hands on an edge, pulling the body up over it
                {
                    float edgeY = r.Y + 34, rise = (float)(e * 20);
                    sb.Color = U.A(Color.FromArgb(120, 60, 120, 70), a); g.FillRectangle(sb, r.X + 4, edgeY, r.Width - 8, r.Bottom - 2 - edgeY);   // the wall
                    g.DrawLine(pen, r.X + 2, edgeY, r.Right - 2, edgeY);                     // its edge
                    float headY = edgeY + 4 - rise;
                    g.DrawEllipse(pen, cx - 8, headY - 8, 16, 16);                          // the climber, rising
                    PointF lh = new PointF(cx - 18, edgeY - 3), rh = new PointF(cx + 18, edgeY - 3);
                    g.DrawLine(pen, cx - 7, headY + 12, lh.X, lh.Y); g.DrawLine(pen, cx + 7, headY + 12, rh.X, rh.Y);
                    sb.Color = U.A(line, a); g.FillEllipse(sb, lh.X - 4, lh.Y - 4, 8, 8); g.FillEllipse(sb, rh.X - 4, rh.Y - 4, 8, 8);   // the hands hold on
                    break;
                }
                default:  // a door swinging open from its handle
                {
                    float sw = (float)(e * 0.55);
                    g.DrawRectangle(pen, r.X + 10, r.Y + 4, 44, r.Height - 8);              // the frame
                    PointF h0 = new PointF(r.X + 12, r.Y + 6), h1 = new PointF(r.X + 12, r.Bottom - 6);
                    float dw = 40 * (float)Math.Cos(sw), dk = 14 * (float)Math.Sin(sw);
                    PointF[] door = { h0, new PointF(h0.X + dw, h0.Y + dk), new PointF(h1.X + dw, h1.Y - dk), h1 };
                    sb.Color = U.A(Color.FromArgb(120, 60, 120, 70), a); g.FillPolygon(sb, door);
                    g.DrawPolygon(pen, door);
                    PointF knob = new PointF(h0.X + dw * 0.85f, r.Y + r.Height / 2f);
                    sb.Color = U.A(line, a); g.FillEllipse(sb, knob.X - 3, knob.Y - 3, 6, 6);
                    g.DrawEllipse(pen, knob.X + 4, knob.Y - 6, 12, 12);                     // the hand on it
                    break;
                }
            }
        }

        // ---------------- tape change ----------------
        void DrawTapeChange(Graphics g, double t)
        {
            double k = t - pageAt;
            if (k < 0 || k > 0.32) return;
            double s = 1 - k / 0.32;
            Tiles(g, staticTiles, 11);
            Fill(g, Color.FromArgb((int)(255 * (1 - s * 0.75)), 0, 0, 0), 0, 0, W, H);
            for (int i = 0; i < 6; i++)
            {
                int y = (int)(U.Hash(i, frame * 13) * H), hh = 2 + (int)(U.Hash(i + 9, frame) * 18);
                Fill(g, Color.FromArgb((int)(120 * s), 220, 255, 225), 0, y, W, hh);
            }
            string tape = page == PageSettings ? "TAPE 02" : page == PageControls ? "TAPE 03" : "TAPE 01";
            TextCenter(g, "▶ " + tape, fBoot, Color.FromArgb((int)(230 * s), 230, 255, 232), W / 2f, H / 2f - 14, true);
        }
    }
}
