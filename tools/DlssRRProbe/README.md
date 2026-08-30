# DLSS Ray Reconstruction A0 probe

This is an isolated Windows/Vulkan hardware-contract probe for the pinned
NVIDIA NGX SDK. It is deliberately not part of `RT2App.sln`: RT2's CPU-only
test and slice-runner targets must not acquire an NGX or Vulkan dependency.

The probe performs the integration steps that are unsafe to infer from SDK
headers alone:

1. asks NGX for the Ray Reconstruction instance extensions before creating
   the Vulkan instance;
2. selects a Vulkan NVIDIA device and asks for device extensions before
   creating the logical device;
3. initializes NGX with RT2's custom project identifier and a writable local
   application-data directory;
4. checks hardware/driver feature support and queries Quality-mode render
   dimensions for a 1280x720 output;
5. creates the feature, supplies real Vulkan image resources for every
   mandatory input, evaluates one reset frame, and reads back the output.

Build and run from the repository root:

```powershell
$build = Join-Path $env:LOCALAPPDATA 'RT2/Build/dlssrr-a0'
cmake -S tools/DlssRRProbe -B $build -A x64
cmake --build $build --config Release
& "$build/Release/RT2DlssRRProbe.exe" --validation
```

The executable returns nonzero on any unsupported extension, NGX error,
feature-creation error, evaluation error, or empty output. NGX writes its own
diagnostic files beneath `%LOCALAPPDATA%/RT2/NGX/a0-probe`.

Useful negative probes are:

```powershell
& "$build/Release/RT2DlssRRProbe.exe" --project-id=
& "$build/Release/RT2DlssRRProbe.exe" --vendor-id=0xffff
& "$build/Release/RT2DlssRRProbe.exe" --simulate-support-mask=2
```

To test a missing runtime, copy only the executable to an empty directory and
run that copy. Passing an empty feature path while the DLLs remain beside the
original executable is not a valid missing-runtime test because NGX also
searches the executable directory.
