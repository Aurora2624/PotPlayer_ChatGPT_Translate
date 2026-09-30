# Offline MSI package

This is a genuine x64 Windows Installer package built with WiX 5.0.2. It installs
exactly these four payload files into one PotPlayer installation:

- `SubtitleTranslate - ChatGPT.as`
- `SubtitleTranslate - ChatGPT.ico`
- `SubtitleTranslate - ChatGPT - Without Context.as`
- `SubtitleTranslate - ChatGPT - Without Context.ico`

The cabinet, complete GPLv3 license and a small native destination-validation DLL
are embedded. Installation requires neither Python, .NET, WiX, internet access,
nor an API key. The DLL only reads filesystem/Windows Installer information and
sets MSI properties or reports errors; standard MSI actions perform all file
changes. There is no EXE installer nested in the MSI and no install-time download.

## Choosing the destination

Close PotPlayer before installation. The wizard shows the GPL license and a
directory picker. Choose the full `Extension\Subtitle\Translate` subfolder of
your existing PotPlayer installation, not the PotPlayer root itself. For example:

```text
C:\Program Files\DAUM\PotPlayer\Extension\Subtitle\Translate
```

Common `DAUM\PotPlayer` and `PotPlayer` locations under Program Files are checked.
An existing installation of this MSI takes precedence. If automatic detection
does not find your installation, use the picker or the `INSTALLFOLDER` property.
The conventional default is not created blindly: immediately before installation
(also in silent mode), setup verifies that the selected folder has the expected
suffix and that its PotPlayer root contains `PotPlayerMini64.exe` or
`PotPlayerMini.exe`. Missing plugin subfolders can be created beneath that verified
root. Network/device paths, symbolic links and junctions are rejected.

An MSI installation is per-machine and needs administrator rights. Only one
PotPlayer destination is supported per machine. Upgrades and repairs retain that
destination; uninstall first if you need to move it.

## Configuration and other installer formats

Both plugin variants are installed with the release's default settings. Restart
PotPlayer, select the desired ChatGPT subtitle translator and configure its
endpoint, model and API key through PotPlayer's translation-plugin settings.
The MSI does not show the EXE installers' API-configuration wizard, store keys or
test a provider. See the main repository README for configuration instructions.

Do not install multiple package formats over the same files. On first install,
the MSI refuses existing same-named files not registered to this MSI. Back up
custom edits, then uninstall the previous EXE installer or move manually copied
plugin files before switching. Merely having another MSI version installed does
not authorize taking over files in a different directory.

Windows Apps & Features supplies repair and uninstall. Uninstall removes this
MSI's four plugin files and its Windows Installer registration, not other plugins,
PotPlayer or API settings stored by PotPlayer. It does not recursively delete any
directory. Missing files can be repaired from the original MSI. Standard MSI
repair rules may preserve user-modified unversioned files; `/fa` intentionally
restores all four release files. Upgrading replaces the old release's payload, so
back up edits made directly to either `.as` file first.

## Build

Use Windows with PowerShell 7, .NET SDK 8 or newer, and Visual Studio C++ Build
Tools with the x64 compiler and Windows SDK. The build needs network access to
official NuGet. From an already staged, version-stamped repository:

```powershell
& .\releases\build\build_msi_installer.ps1 -Version v1.9.5 -OutputDir C:\artifacts
```

The output is `C:\artifacts\installer.msi`. `Version` and an absolute `OutputDir`
are required. The repository root is derived from the builder's location; it must
run from the staged repository so its root `.as` files already report `1.9.5`.
The builder verifies that stamp rather than editing source files.

WiX CLI and `WixToolset.UI.wixext` are both pinned to **5.0.2** and downloaded from
NuGet into a disposable build directory. No global WiX tool or extension is
installed. WiX 5 is intentionally retained to avoid introducing WiX 6+'s new
package maintenance-fee agreement. The native validator uses the x64 compiler and
static C runtime. The license RTF is generated from the staged root `LICENSE`;
its wording is not replaced with WiX's placeholder license. The resulting MSI is
opened as an MSI database and its File table must contain exactly the four
expected plugin files before it is copied to `OutputDir`.

### Version policy

MSI accepts `MAJOR.MINOR.PATCH` versions, optionally prefixed with `v`, and SemVer
prerelease labels such as `v1.9.5-rc.1`. Numeric identifiers cannot have leading
zeros. Major/minor must each be 0–255 and patch 0–65535; the complete input is
limited to 128 characters. `0.0.0` is suitable for non-release CI previews. Build
metadata and fourth numeric fields are rejected.

Windows Installer's numeric ProductVersion is `1.9.5` for both `1.9.5-rc.1` and
`1.9.5`, while the product display name, identity and embedded scripts retain the
full release version. Different releases sharing that numeric base are explicitly
blocked from co-installation or automatic replacement. Back up edits and uninstall
the existing MSI first when moving from `1.9.5-rc.1` to `1.9.5-rc.2` or `1.9.5`,
in either direction. This is necessary because MSI cannot order SemVer labels.

The UpgradeCode and each one-file component GUID remain stable. ProductCode is
UUID v5 derived from UpgradeCode + full release version, so rebuilding the
same version does not create a second product registration. A later numeric
version performs a transactional major upgrade; downgrades are blocked. Removal
of the older product is scheduled after `InstallInitialize`, allowing rollback
if the new installation fails. Publish changed payloads under a new version.

## Windows smoke-test checklist

Run in a disposable Windows VM with an actual PotPlayer installation, then repeat
with a portable PotPlayer directory. Keep verbose MSI logs for failures.

```powershell
# Interactive license and destination selection
msiexec.exe /i C:\artifacts\installer.msi /l*v C:\artifacts\install.log

# Silent install into an explicitly chosen existing PotPlayer installation
msiexec.exe /i C:\artifacts\installer.msi /qn /norestart 'INSTALLFOLDER=C:\Program Files\DAUM\PotPlayer\Extension\Subtitle\Translate' /l*v C:\artifacts\silent.log

# Restore all release files after deleting one; overwrites local script edits
msiexec.exe /fa C:\artifacts\installer.msi /qn /norestart /l*v C:\artifacts\repair.log

# Uninstall; unrelated sentinel files must survive
msiexec.exe /x C:\artifacts\installer.msi /qn /norestart /l*v C:\artifacts\uninstall.log
```

Check all of the following before calling installation behavior verified:

1. MSI build and ICE validation succeed on Windows; package File table has only
   the four expected files and SummaryInformation uses x64.
2. Cancel on license/directory/confirmation pages changes no payload files.
3. Selecting a nonexistent PotPlayer root fails without creating fake app folders.
4. Existing manual/EXE-managed same-named files are rejected without modification.
5. Install writes all four staged hashes; launch PotPlayer and select each plugin.
6. Repair restores a deleted plugin; full repair restores deliberately edited files.
7. Install a higher numeric test version into the same directory; only one Apps &
   Features entry remains and the new payload is present. Lower numeric versions
   and different prerelease/stable releases with the same numeric base are rejected.
8. Uninstall removes the four payload files while unrelated translator/sentinel
   files and the PotPlayer executable remain. Uninstall also succeeds after the
   PotPlayer directory was removed independently.

Linux XML/static checks are not a substitute for these Windows lifecycle tests.

## References

- [WiX 5.0.2 on official NuGet](https://www.nuget.org/packages/wix/5.0.2)
- [WiX UI and custom license/localization](https://docs.firegiant.com/wix/tools/wixext/wixui/)
- [WiX major-upgrade sequencing](https://docs.firegiant.com/wix/schema/wxs/majorupgrade/)
- [Windows Installer ProductVersion limits](https://learn.microsoft.com/en-us/windows/win32/msi/productversion)
