# PC setup for Rim3D

One-time setup on sneed's Windows PC. Takes about an hour, mostly waiting for downloads
(about 100 GB). Do the steps in order. Anything in a grey box can be copied with the copy
button on its top right.

## 1. Install the Epic Games Launcher

Open **PowerShell** (press the Windows key, type `powershell`, press Enter) and paste:

```powershell
winget install --id EpicGames.EpicGamesLauncher -e
```

Open the launcher and sign in with your Epic account (create one if you don't have one).

## 2. Install Unreal Engine 5

In the Epic Games Launcher:

1. Click **Unreal Engine** on the left, then the **Library** tab at the top.
2. Click the **+** next to **Engine Versions**. Pick the newest **5.x** version in the dropdown.
3. Click **Install**, keep the default location, then click **Options** and change these to save
   about 70 GB:
   - Untick **Editor symbols for debugging**.
   - Under **Target Platforms**, untick **Android**, **iOS** and **Linux**.
4. Click **Apply**, then **Install**. This takes a while; carry on with the next steps meanwhile.

## 3. Install Visual Studio (the C++ compiler)

Paste into PowerShell. Click **Yes** if Windows asks for permission:

```powershell
winget install --id Microsoft.VisualStudio.2022.Community -e --override "--add Microsoft.VisualStudio.Workload.NativeGame --add Microsoft.VisualStudio.Workload.NativeDesktop --add Microsoft.VisualStudio.Workload.ManagedDesktop --includeRecommended --passive --norestart"
```

You never need to open Visual Studio yourself.

## 4. Install Git and Git LFS

Paste into PowerShell:

```powershell
winget install --id Git.Git -e
winget install --id GitHub.GitLFS -e
```

Then **close PowerShell, open a new one**, and paste:

```powershell
git lfs install
mkdir C:\Dev
```

## 5. Tell Windows Defender to skip the project folders

This stops Defender scanning every file during builds, which causes slowdowns and pop-ups.
It only affects these two folders.

Press the Windows key, type `powershell`, **right-click Windows PowerShell and choose
Run as administrator**, click **Yes**, then paste:

```powershell
Add-MpPreference -ExclusionPath "C:\Dev"
Add-MpPreference -ExclusionPath "C:\Program Files\Epic Games"
```

## 6. Get Epic's Game Animation Sample (free)

This gives the protagonist realistic movement.

1. When Unreal Engine has finished installing, go to **Unreal Engine → Samples** in the launcher.
2. Find **Game Animation Sample** and click it, then **Create Project**.
3. Set the folder to `C:\Dev` and the name to `GameAnimationSample`. If it asks for an engine
   version, pick the one you installed in step 2. If yours isn't offered, install the newest
   version it does offer (step 2 again) and use that.
4. Click **Create**. When it's done you can close it without opening it.

## 7. Let Claude work on your PC

1. Open the Claude desktop app and go to **Settings → Remote Control**, and turn it on for this PC.
2. Make sure the PC can stay on for about an hour, with no game or local AI tool running.
3. Send me this message in the thread:

```
PC ready
```

I'll then download the repo to `C:\Dev\Rim3D`, limit builds so they don't hog your CPU,
compile the game, bring in the animation sample, and send you a screenshot.
