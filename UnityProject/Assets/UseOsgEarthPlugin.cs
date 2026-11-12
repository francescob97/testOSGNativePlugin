using UnityEngine;
using UnityEngine.Rendering;
using System;
using System.Collections;
using System.Runtime.InteropServices;
using System.Text;


// Shows the osgEarth viewer (OpenGL, see PluginSource/source/OsgEarthViewer.cpp) inside Unity
// running on Direct3D 11.
//
// The native plugin renders osgEarth into the Texture2D created here, once per frame, from
// Unity's render thread (GL.IssuePluginEvent). This script then draws that texture on screen
// and forwards mouse/keyboard input to osgEarth's EarthManipulator.
public class UseOsgEarthPlugin : MonoBehaviour
{
    [Tooltip("Arguments of osgearth_viewer.exe: an .earth file followed by options, e.g. \"simple.earth --sky --samples 4\". " +
             "Relative paths are searched in StreamingAssets too. Use double quotes for paths with spaces.")]
    public string commandLine = "osgearth/simple.earth";

    [Tooltip("Size of the texture osgEarth renders into. 0 = follow the on-screen size of the viewer.")]
    public int width = 0;
    public int height = 0;

    [Tooltip("Draw the osgEarth image on screen and send it mouse and keyboard input.")]
    public bool drawOnScreen = true;

    [Tooltip("Where to draw it, in normalized screen coordinates (0,0 = top-left).")]
    public Rect screenRect = new Rect(0, 0, 1, 1);

    [Tooltip("Turn on if the image appears upside down.")]
    public bool flipVertically = false;

    [Tooltip("Optional: also put the osgEarth image on this renderer's material.")]
    public Renderer targetRenderer;

    [Tooltip("Copy osgEarth/OpenSceneGraph messages to the Unity console.")]
    public bool showLog = true;

    // The texture osgEarth renders into (null until Play)
    public Texture2D OsgEarthTexture { get; private set; }

    // Event IDs understood by OnRenderEvent in RenderingPlugin.cpp
    const int kRenderEvent = 3;
    const int kShutdownEvent = 4;

    // Values of MouseEventType / KeyEventType in OsgEarthViewer.cpp
    const int kMouseMove = 0, kMousePress = 1, kMouseRelease = 2, kMouseScroll = 3, kMouseDoubleClick = 4;
    const int kKeyPress = 0, kKeyRelease = 1;

    [DllImport("RenderingPlugin")]
    private static extern IntPtr GetRenderEventFunc();

    [DllImport("RenderingPlugin")]
    private static extern void OsgEarth_SetCommandLine(byte[] utf8CommandLine, byte[] utf8DataPath);

    [DllImport("RenderingPlugin")]
    private static extern void OsgEarth_SetTexture(IntPtr texture, int w, int h);

    [DllImport("RenderingPlugin")]
    private static extern void OsgEarth_MouseEvent(int type, float x, float y, int value);

    [DllImport("RenderingPlugin")]
    private static extern void OsgEarth_KeyEvent(int type, int key);

    [DllImport("RenderingPlugin")]
    private static extern IntPtr OsgEarth_GetStatus();

    [DllImport("RenderingPlugin")]
    private static extern IntPtr OsgEarth_PopLogMessage();

    string m_Error;          // shown on screen when the plugin cannot run
    bool m_Running;
    int m_PressedButtons;    // bit mask of the Unity mouse buttons pressed over the viewer
    Coroutine m_Loop;

    void OnEnable()
    {
        m_Error = null;
        if (SystemInfo.graphicsDeviceType != GraphicsDeviceType.Direct3D11)
        {
            m_Error = "The osgEarth viewer needs Direct3D 11 (current: " + SystemInfo.graphicsDeviceType + ")";
            Debug.LogWarning(m_Error);
            return;
        }

        try
        {
            OsgEarth_SetCommandLine(ToUtf8(commandLine), ToUtf8(Application.streamingAssetsPath));
            Vector2Int size = DesiredTextureSize();
            CreateTexture(size.x, size.y);
        }
        catch (Exception e) // DllNotFoundException / EntryPointNotFoundException
        {
            m_Error = "RenderingPlugin.dll without osgEarth support: build PluginSource/projects/VisualStudio2022 (x64) first.\n" + e.Message;
            Debug.LogError(m_Error);
            return;
        }

        m_Running = true;
        m_Loop = StartCoroutine(CallPluginAtEndOfFrames());
    }

    void OnDisable()
    {
        if (m_Running)
        {
            // Queued on the render thread like the render events, so it runs after the last frame.
            // The plugin holds its own reference to the texture, destroying it here is safe.
            GL.IssuePluginEvent(GetRenderEventFunc(), kShutdownEvent);
            m_Running = false;
        }
        if (m_Loop != null)
        {
            StopCoroutine(m_Loop);
            m_Loop = null;
        }
        if (OsgEarthTexture != null)
        {
            Destroy(OsgEarthTexture);
            OsgEarthTexture = null;
        }
        m_PressedButtons = 0;
    }

    Vector2Int DesiredTextureSize()
    {
        int w = width > 0 ? width : Mathf.RoundToInt(screenRect.width * Screen.width);
        int h = height > 0 ? height : Mathf.RoundToInt(screenRect.height * Screen.height);
        return new Vector2Int(Mathf.Max(w, 16), Mathf.Max(h, 16));
    }

    // Creates the texture osgEarth renders into and hands it to the plugin. Can be called again
    // to resize: the plugin keeps a reference to the old texture until its render thread switches.
    void CreateTexture(int w, int h)
    {
        Texture2D old = OsgEarthTexture;
        // RGBA32 maps to DXGI_FORMAT_R8G8B8A8_UNORM(_SRGB), which the plugin can share with OpenGL.
        // linear = false: sRGB sampling in Linear color space projects (osgEarth outputs sRGB colors).
        OsgEarthTexture = new Texture2D(w, h, TextureFormat.RGBA32, false, false);
        OsgEarthTexture.name = "osgEarth";
        OsgEarthTexture.wrapMode = TextureWrapMode.Clamp;
        OsgEarthTexture.Apply(false, true); // upload to the GPU, no CPU copy needed
        if (targetRenderer != null)
            targetRenderer.material.mainTexture = OsgEarthTexture;

        // Same idea as SetTextureFromUnity in UseRenderingPlugin
        OsgEarth_SetTexture(OsgEarthTexture.GetNativeTexturePtr(), w, h);

        if (old != null)
            Destroy(old);
    }

    IEnumerator CallPluginAtEndOfFrames()
    {
        while (true)
        {
            // Same pattern as UseRenderingPlugin: render after Unity is done with the frame.
            yield return new WaitForEndOfFrame();
            GL.IssuePluginEvent(GetRenderEventFunc(), kRenderEvent);
        }
    }

    void Update()
    {
        if (!m_Running)
            return;

        // Follow the Game view / window size
        Vector2Int size = DesiredTextureSize();
        if (size.x != OsgEarthTexture.width || size.y != OsgEarthTexture.height)
            CreateTexture(size.x, size.y);

        if (!showLog)
            return;
        for (IntPtr msg = OsgEarth_PopLogMessage(); msg != IntPtr.Zero; msg = OsgEarth_PopLogMessage())
        {
            string text = FromUtf8(msg);
            if (text.StartsWith("[W] "))
                Debug.LogWarning("[osgEarth] " + text.Substring(4));
            else
                Debug.Log("[osgEarth] " + text);
        }
    }

    void OnGUI()
    {
        if (m_Error != null)
        {
            GUI.Label(new Rect(10, 10, Screen.width - 20, 80), m_Error);
            return;
        }
        if (!m_Running || OsgEarthTexture == null)
            return;

        Rect r = new Rect(screenRect.x * Screen.width, screenRect.y * Screen.height,
                          screenRect.width * Screen.width, screenRect.height * Screen.height);

        if (drawOnScreen)
        {
            if (Event.current.type == EventType.Repaint)
            {
                if (flipVertically)
                    GUI.DrawTextureWithTexCoords(r, OsgEarthTexture, new Rect(0, 1, 1, -1));
                else
                    GUI.DrawTexture(r, OsgEarthTexture, ScaleMode.StretchToFill, false);
            }
            ForwardInput(Event.current, r);
        }

        string status = FromUtf8(OsgEarth_GetStatus());
        if (!status.StartsWith("Running"))
            GUI.Label(new Rect(r.x + 10, r.y + 10, r.width - 20, 60), "osgEarth: " + status);
    }

    // IMGUI events have the origin at the top-left corner, like osgGA with Y_INCREASING_DOWNWARDS.
    void ForwardInput(Event e, Rect r)
    {
        Vector2 p = e.mousePosition;
        float x = (p.x - r.x) / r.width * OsgEarthTexture.width;
        float y = (p.y - r.y) / r.height * OsgEarthTexture.height;
        if (flipVertically)
            y = OsgEarthTexture.height - y;
        bool inside = r.Contains(p);

        switch (e.type)
        {
            case EventType.MouseDown:
                if (!inside) break;
                m_PressedButtons |= 1 << e.button;
                OsgEarth_MouseEvent(e.clickCount == 2 ? kMouseDoubleClick : kMousePress, x, y, ToOsgButton(e.button));
                e.Use();
                break;
            case EventType.MouseUp:
                if ((m_PressedButtons & (1 << e.button)) == 0) break;
                m_PressedButtons &= ~(1 << e.button);
                OsgEarth_MouseEvent(kMouseRelease, x, y, ToOsgButton(e.button));
                e.Use();
                break;
            case EventType.MouseDrag:
            case EventType.MouseMove:
                if (!inside && m_PressedButtons == 0) break;
                OsgEarth_MouseEvent(kMouseMove, x, y, 0);
                break;
            case EventType.ScrollWheel:
                if (!inside) break;
                // Unity: delta.y < 0 when the wheel moves away from the user (= osgGA SCROLL_UP)
                OsgEarth_MouseEvent(kMouseScroll, x, y, e.delta.y < 0 ? 1 : -1);
                e.Use();
                break;
            case EventType.KeyDown:
                if (e.character != '\0')
                {
                    // printable keys (osgGA uses the character code)
                    OsgEarth_KeyEvent(kKeyPress, e.character);
                    OsgEarth_KeyEvent(kKeyRelease, e.character);
                }
                else if (ToOsgKey(e.keyCode) != 0)
                    OsgEarth_KeyEvent(kKeyPress, ToOsgKey(e.keyCode));
                break;
            case EventType.KeyUp:
                if (ToOsgKey(e.keyCode) != 0)
                    OsgEarth_KeyEvent(kKeyRelease, ToOsgKey(e.keyCode));
                break;
        }
    }

    // Unity: 0 left, 1 right, 2 middle. osgGA: 1 left, 2 middle, 3 right.
    static int ToOsgButton(int unityButton)
    {
        return unityButton == 0 ? 1 : unityButton == 1 ? 3 : 2;
    }

    // Non-printable keys used by the osgEarth/osgGA manipulators (osgGA::GUIEventAdapter::KeySymbol)
    static int ToOsgKey(KeyCode k)
    {
        switch (k)
        {
            case KeyCode.LeftArrow: return 0xFF51;
            case KeyCode.UpArrow: return 0xFF52;
            case KeyCode.RightArrow: return 0xFF53;
            case KeyCode.DownArrow: return 0xFF54;
            case KeyCode.Home: return 0xFF50;
            case KeyCode.PageUp: return 0xFF55;
            case KeyCode.PageDown: return 0xFF56;
            case KeyCode.LeftShift: return 0xFFE1;
            case KeyCode.RightShift: return 0xFFE2;
            case KeyCode.LeftControl: return 0xFFE3;
            case KeyCode.RightControl: return 0xFFE4;
            case KeyCode.LeftAlt: return 0xFFE9;
            case KeyCode.RightAlt: return 0xFFEA;
            default: return 0;
        }
    }

    static byte[] ToUtf8(string s)
    {
        return Encoding.UTF8.GetBytes((s ?? "") + "\0");
    }

    static string FromUtf8(IntPtr p)
    {
        if (p == IntPtr.Zero)
            return "";
        int len = 0;
        while (Marshal.ReadByte(p, len) != 0)
            ++len;
        byte[] bytes = new byte[len];
        Marshal.Copy(p, bytes, 0, len);
        return Encoding.UTF8.GetString(bytes);
    }
}
