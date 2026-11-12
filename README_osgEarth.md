# osgEarth viewer dentro Unity (Direct3D 11)

Questo fork di [NativeRenderingPlugin](https://github.com/Unity-Technologies/NativeRenderingPlugin)
integra il viewer OpenGL di osgEarth
([`osgearth_viewer.cpp`](https://github.com/pelicanmapping/osgearth/blob/master/src/applications/osgearth_viewer/osgearth_viewer.cpp))
in un progetto Unity 2022.3 che usa Direct3D 11. Il resto del sample originale (triangolo,
texture "plasma", mesh animata) è invariato e continua a funzionare.

## Architettura

```
 Unity main thread (C#)                 Unity render thread (plugin C++)                 GPU
 ─────────────────────                  ────────────────────────────────                 ───
 UseOsgEarthPlugin.cs                   RenderingPlugin.cpp  OnRenderEvent(3)
  ├ crea Texture2D RGBA32  ─ptr────────► OsgEarthViewer.cpp
  ├ OsgEarth_SetCommandLine(...)          ├ contesto OpenGL nascosto (WGL, finestra mai mostrata)
  ├ input mouse/tastiera ─coda──────────► ├ osgViewer::Viewer "embedded" (port di osgearth_viewer)
  ├ GL.IssuePluginEvent(3) ogni frame     │    viewer.frame() disegna in un FBO OpenGL
  └ OnGUI: disegna la texture             ├ FBO color = texture D3D11 condivisa  ◄── WGL_NV_DX_interop2
                                          │    (fallback: glReadPixels + UpdateSubresource)
                                          └ CopySubresourceRegion(shared → Texture2D di Unity)
```

1. **Contesto OpenGL proprio.** Unity gira su D3D11, osgEarth richiede OpenGL: il plugin crea sul
   render thread di Unity una finestra Win32 invisibile e un contesto OpenGL (compatibility
   profile, quello che OSG/osgEarth si aspettano). Il contesto viene reso corrente solo durante
   gli eventi del plugin e poi rilasciato.
2. **Viewer embedded.** `osgearth_viewer.cpp` è portato quasi riga per riga (`CreateViewer()`):
   stessa riga di comando (`osg::ArgumentParser`), `osgEarth::initialize`, `EarthManipulator`,
   `MapNodeHelper().load(...)`, ramo `--light`, `ShaderGenerator`. Le differenze:
   `setUpViewerAsEmbeddedInWindow()` al posto della finestra OSG, e un `viewer.frame()` per
   ogni frame Unity al posto di `viewer.run()`.
3. **Rendering in FBO.** Prima di `frame()` il plugin binda il proprio FBO e lo registra come
   "default FBO" del contesto OSG (`GraphicsContext::setDefaultFboId`), così sia la camera
   principale sia eventuali camere RTT di osgEarth tornano a disegnare lì.
4. **Condivisione con D3D11.** Il color buffer dell'FBO è una texture D3D11 condivisa con
   OpenGL tramite `WGL_NV_DX_interop2` (supportata da NVIDIA, AMD e Intel): niente copie su
   CPU. Ogni frame: lock → OSG disegna → unlock → copia GPU nella `Texture2D` di Unity.
   Se l'interop non è disponibile (es. portatili ibridi con GL e D3D su GPU diverse) si passa
   automaticamente a `glReadPixels` + `UpdateSubresource` (più lento, ma funziona ovunque).
5. **Input.** `OnGUI` converte gli eventi Unity in coordinate pixel della texture e li accoda;
   il render thread li inserisce nella `osgGA::EventQueue` della finestra embedded prima di
   ogni frame, quindi `EarthManipulator` funziona come nel viewer originale (trascinamento,
   rotella, doppio click, frecce, `space` per la home, `s` per le statistiche...).
6. **Log.** I messaggi di OSG/osgEarth vengono raccolti dal plugin e mostrati nella Console di
   Unity (prefisso `[osgEarth]`).

## Cosa è stato modificato rispetto all'upstream

| File | Modifica |
|------|----------|
| `PluginSource/source/OsgEarthViewer.cpp` | **nuovo**: contesto GL, interop D3D11, port di `osgearth_viewer` |
| `PluginSource/source/RenderingPlugin.cpp` | +17 righe: eventi `3` (frame osgEarth) e `4` (shutdown), cleanup allo shutdown del device |
| `PluginSource/projects/VisualStudio2022/osgearth.props` | **nuovo**: include/lib di OSG+osgEarth (solo x64) |
| `PluginSource/projects/VisualStudio2022/RenderingPlugin.vcxproj(.filters)` | +1 sorgente, +1 import di `osgearth.props` |
| `UnityProject/Assets/UseOsgEarthPlugin.cs` | **nuovo**: texture, eventi di rendering, input, log |
| `UnityProject/Assets/scene.unity` | componente `UseOsgEarthPlugin` aggiunto alla Main Camera |
| `UnityProject/Assets/StreamingAssets/osgearth/simple.earth` | **nuovo**: mappa di esempio (ReadyMap, richiede internet) |
| `UnityProject/ProjectSettings/ProjectVersion.txt`, `Packages/manifest.json` | Unity 2022.3 invece di 2023.1 |
| `StartUnityWithOsgEarth.bat` | **nuovo**: avvia l'Editor in D3D11 con PATH/dati di OSG e osgEarth |

Tutto il codice osgEarth è dentro `#if SUPPORT_OSGEARTH`, definito solo dalle build x64 di
Visual Studio: le build Win32/Linux/macOS/Android/UWP del sample restano identiche.

## Requisiti

* Windows 10/11 x64, Visual Studio 2022 (toolset v143), Unity 2022.3 LTS.
* OpenSceneGraph 3.6.5 + osgEarth 3.x (testato sulle API di 3.7.2, 3.8.1 e master). Il modo più
  semplice è vcpkg:

  ```bat
  git clone https://github.com/microsoft/vcpkg C:\vcpkg
  C:\vcpkg\bootstrap-vcpkg.bat
  C:\vcpkg\vcpkg install osgearth:x64-windows
  ```

  Va bene anche una build CMake propria di OSG + osgEarth, purché compilata x64 con runtime
  `/MD` (release) e installata in una cartella con `include\`, `lib\`, `bin\`.

## Build del plugin

1. Imposta la variabile d'ambiente `OSGEARTH_SDK` (es. `C:\vcpkg\installed\x64-windows`) oppure
   modifica il default in `PluginSource/projects/VisualStudio2022/osgearth.props`.
2. Apri `PluginSource/projects/VisualStudio2022/RenderingPlugin.sln`, configura **Release | x64**,
   compila. Il post-build copia `RenderingPlugin.dll` in `UnityProject/Assets/Plugins/x86_64/`
   (sostituendo la DLL precompilata dell'upstream, che non contiene osgEarth).

## Avvio in Unity

`RenderingPlugin.dll` dipende dalle DLL di OSG/osgEarth (e dalle loro dipendenze: GDAL, PROJ,
curl, ...), che Windows deve trovare quando Unity carica il plugin. Due possibilità:

* **`StartUnityWithOsgEarth.bat`** (consigliato): imposta `PATH`, `OSG_LIBRARY_PATH` (plugin
  `osgPlugins-3.6.5`), `PROJ_DATA`/`PROJ_LIB` e `GDAL_DATA`, poi apre il progetto con
  `-force-d3d11`. Imposta `UNITY_EDITOR` se Unity non è in
  `C:\Program Files\Unity\Hub\Editor\2022.3.62f1\`.
* oppure imposta le stesse variabili a livello di sistema e apri il progetto dall'Hub.

Premi Play: il globo occupa la Game view. Il primo frame impiega qualche secondo
(inizializzazione di osgEarth e caricamento della mappa); lo stato compare in alto a sinistra
finché il viewer non è attivo, i messaggi completi sono nella Console.

## Componente `UseOsgEarthPlugin`

| Campo | Significato |
|-------|-------------|
| Command Line | Gli stessi argomenti di `osgearth_viewer.exe`, es. `osgearth/simple.earth --sky --samples 4`. I percorsi relativi sono cercati anche in `StreamingAssets`. |
| Width / Height | Risoluzione della texture; `0` = segue la dimensione a schermo. |
| Draw On Screen / Screen Rect | Disegna l'immagine (e riceve l'input) nel rettangolo indicato, coordinate normalizzate con origine in alto a sinistra. |
| Target Renderer | Opzionale: usa l'immagine anche come texture del materiale di un oggetto. |
| Flip Vertically | Se l'immagine dovesse risultare capovolta. |
| Show Log | Copia i messaggi OSG/osgEarth nella Console. |

`OsgEarthTexture` è pubblica, quindi la texture si può usare anche da altri script (UI, materiali).

## Build standalone (player)

Nella cartella del gioco servono, accanto all'exe: le DLL di `%OSGEARTH_SDK%\bin`, la cartella
`osgPlugins-3.6.5` (il plugin la cerca anche accanto all'exe e accanto a `RenderingPlugin.dll`)
e i dati PROJ/GDAL con le relative variabili d'ambiente. Il player deve usare Direct3D 11
(primo API grafica in *Player Settings → Other Settings*, già così in questo progetto).

## Limiti noti

* Una sola istanza del viewer per processo (un solo `UseOsgEarthPlugin` attivo).
* Funziona solo con Direct3D 11 su Windows x64; con altre API il componente mostra un messaggio.
* Il primo frame blocca il render thread durante il caricamento della mappa.
* Il codice è stato verificato compilandolo con gli header reali di OSG 3.6.5 e osgEarth
  (3.7.2 e master) ma non ancora eseguito: alla prima prova controllare la Console Unity, che
  riporta driver OpenGL in uso, modalità (interop o readback) ed eventuali errori di osgEarth.
