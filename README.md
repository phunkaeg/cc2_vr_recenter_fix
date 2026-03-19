# CC2 VR Recenter Fix

A small community-made fix for **Carrier Command 2 VR** that corrects the game's recenter behaviour when the player’s head is tilted.

## Why this exists

In the current VR implementation of *Carrier Command 2*, pressing the recenter button while your head is pitched or rolled can cause that tilt to become baked into the new horizon. That makes the world feel rotated incorrectly, can shift the player in odd directions, and can be quite nausea-inducing.

This fix was created to correct that behaviour by removing **pitch and roll** from the recenter basis and recomputing the translation so the reset position behaves properly.

The hope is that the developers will eventually add a proper native fix, making this workaround unnecessary.

---

## What this fixes

This mod changes VR recenter so that:

- **pitch is removed**
- **roll is removed**
- **translation is recomputed** after the corrected orientation is applied

In practice, this means:
- recentering no longer tilts the horizon when your head is angled
- recentering no longer pushes you strangely forward, backward, sideways, or downward because of head tilt
- normal in-game VR tracking remains otherwise unchanged

---

## Release installation

### Files included in the release ZIP

- `cc2_vr_recenter_fix_launcher.exe`
- `cc2_recenter_hook_dll.dll`
- `launch_cc2_vr_recenter_fix.bat`

### Installation

1. Open your **Carrier Command 2** game folder.
2. Place the release files in the same folder as:

   `carrier_command_vr.exe`

3. Launch the fix using either:
   - `cc2_vr_recenter_fix_launcher.exe`
   - or `launch_cc2_vr_recenter_fix.bat`

That’s it.

### Notes

- This fix targets the **32-bit VR executable**.
- The launcher is designed to find `carrier_command_vr.exe` in the same directory.
- The DLL name is expected by the launcher and should not be changed unless you also rebuild the source accordingly.

---

## Building from source

This project was built in **Visual Studio 2022** as **Win32**.

### Requirements

- Visual Studio 2022
- **Desktop development with C++** workload installed
- Build target set to **Win32** (not x64)

### Source projects

The repository includes:
- the launcher/injector source
- the hook DLL source
- Visual Studio solution/project files

### Basic build steps

1. Open the solution in **Visual Studio 2022**
2. Make sure both projects are set to:
   - **Configuration:** `Release`
   - **Platform:** `Win32`
3. Build the solution
4. Copy the resulting files into the game directory next to `carrier_command_vr.exe`

### Important

The game’s VR executable is **32-bit**, so both the launcher and DLL must also be built as **Win32**.

---

## How this was worked out

This fix was produced by reverse engineering the VR recenter path in `carrier_command_vr.exe`.

The investigation traced:

- the VR input action used for recenter
- the digital action polling path
- the pressed-edge logic that triggers recenter
- the function that applies the recenter transform
- the function that builds the current VR view transform from the compositor pose

The key issue turned out to be that the game was using the **full current VR pose basis** when recentering, including head **pitch** and **roll**, instead of using a levelled basis. That incorrect basis was also affecting the translation solve, which explains why recentering while tilted could move the player in strange directions.

This fix hooks the recenter path and corrects the basis before the final transform is used.

---

## Transparency

People are right to be cautious about downloading random executables from the internet.

That’s why:
- the source code is included here
- the release binaries are provided only for convenience
- anyone who prefers can inspect and build the project themselves

---

## Personal note

I am **not a programmer by trade**, and coding is not a skillset I normally work in.

This project would not have been possible without **ChatGPT**, which was instrumental in helping trace the relevant code paths, interpret the reverse engineering process, and build the working fix.

---

## Disclaimer

This is an unofficial community fix. Use it at your own risk.

It is intended as a temporary workaround until an official fix exists.

---

## Credits

- Geometa / the *Carrier Command 2* developers for the original game
- The VR and modding community for highlighting the issue
- ChatGPT for helping guide the reverse engineering and implementation process
