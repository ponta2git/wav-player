# Loaded by verify.ps1, which provides Invoke-Checked, Get-CheckedOutput and
# Assert-Rejected. Audits what the built executables declare about themselves:
# the embedded application manifest, the version information, and the PE headers
# and imports of the Release product.

# Checks the embedded application manifest of Executable and returns the version
# of the manifest's own assemblyIdentity.
function Assert-ApplicationManifest {
    param([string]$Executable, [string]$ManifestTool)
    $extracted = "$Executable.embedded.manifest"
    Invoke-Checked $ManifestTool @('/nologo', "-inputresource:$Executable;#1", "-out:$extracted") | Out-Host
    $manifest = [xml]::new()
    $manifest.Load($extracted)
    $namespaces = [Xml.XmlNamespaceManager]::new($manifest.NameTable)
    $namespaces.AddNamespace('asm', 'urn:schemas-microsoft-com:asm.v1')
    $namespaces.AddNamespace('asmv3', 'urn:schemas-microsoft-com:asm.v3')
    $namespaces.AddNamespace('windows', 'http://schemas.microsoft.com/SMI/2016/WindowsSettings')
    $namespaces.AddNamespace('compat', 'urn:schemas-microsoft-com:compatibility.v1')
    $identity = '/asm:assembly/asm:assemblyIdentity[@type="win32" and @name="Wavplay.WindowsHost" and @processorArchitecture="amd64"]'
    $required = @(
        $identity,
        '//asmv3:windowsSettings/windows:dpiAwareness[text()="PerMonitorV2"]',
        '//asmv3:windowsSettings/windows:longPathAware[text()="true"]',
        '//asm:dependentAssembly/asm:assemblyIdentity[@name="Microsoft.Windows.Common-Controls" and @version="6.0.0.0" and @processorArchitecture="amd64" and @publicKeyToken="6595b64144ccf1df"]',
        '//compat:application/compat:supportedOS[@Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"]',
        '//asmv3:requestedExecutionLevel[@level="asInvoker" and @uiAccess="false"]'
    )
    foreach ($query in $required) {
        if ($manifest.SelectNodes($query, $namespaces).Count -ne 1) {
            throw "Embedded Windows application contract missing or duplicated in ${Executable}: $query"
        }
    }
    $manifest.SelectSingleNode($identity, $namespaces).GetAttribute('version')
}

# The version information of the Release executable (src/app/wavplay.rc) names the
# product and carries the single product version that build/app.manifest's
# assemblyIdentity repeats, in its strings and in its fixed numbers.
function Assert-VersionInfo {
    param([string]$Executable, [string]$ManifestVersion)
    $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($Executable)
    $versions = @($info.FileVersion, $info.ProductVersion,
        "$($info.FileMajorPart).$($info.FileMinorPart).$($info.FileBuildPart).$($info.FilePrivatePart)",
        "$($info.ProductMajorPart).$($info.ProductMinorPart).$($info.ProductBuildPart).$($info.ProductPrivatePart)")
    if ($info.ProductName -cne 'WAV Player' -or $info.FileDescription -cne 'WAV Player' -or
        @($versions | Where-Object { $_ -cne $ManifestVersion }).Count) {
        throw "The version information of $Executable (product '$($info.ProductName)', description '$($info.FileDescription)', versions $($versions -join ', ')) does not name WAV Player with the version $ManifestVersion of build/app.manifest; src/app/wavplay.rc and the manifest change together."
    }
}

# dumpbin /headers prints a flags field as a line of its hexadecimal value and
# its name, such as '8160 DLL characteristics'. The pattern matches that line
# for Field with the indentation, the value and the rest as groups 1 to 3.
function Get-HeaderFieldPattern {
    param([string]$Field)
    "^(\s*)([0-9A-Fa-f]+)( $Field\s*)$"
}

# Returns the value of every line of Headers that prints the flags field Field.
function Get-HeaderField {
    param([string[]]$Headers, [string]$Field)
    $pattern = Get-HeaderFieldPattern $Field
    @($Headers | ForEach-Object {
        if ($_ -match $pattern) { [Convert]::ToUInt32($Matches[2], 16) }
    })
}

# Returns a copy of Headers in which the flags field Field has the bits Set
# added and the bits Clear removed, for the self-check.
function Set-HeaderFieldBits {
    param([string[]]$Headers, [string]$Field, [UInt32]$Set, [UInt32]$Clear)
    $pattern = Get-HeaderFieldPattern $Field
    $Headers | ForEach-Object {
        if ($_ -match $pattern) {
            $value = [Convert]::ToUInt32($Matches[2], 16) -bor $Set
            '{0}{1:X}{2}' -f $Matches[1], ($value - ($value -band $Clear)), $Matches[3]
        } else { $_ }
    }
}

# In /dependents output the only indented one-word lines with a dot are the
# imported module names.
function Test-ImportLine {
    param([string]$Line)
    $Line -match '^\s+\S+\.\S+\s*$'
}

# Checks dumpbin /headers and /dependents output of the Release executable against
# the documented build decisions and returns the violations. It reads only PE header
# flags, debug-directory entry types and DLL names, so it does not depend on how a
# particular toolset generates or inlines code.
function Test-ReleaseHeaders {
    param([string[]]$Headers, [string[]]$Dependents)
    $failures = [Collections.Generic.List[string]]::new()
    if (-not ($Headers -match '8664 machine \(x64\)') -or -not ($Headers -match '2 subsystem \(Windows GUI\)')) {
        $failures.Add('Expected an x64 Windows GUI release executable.')
    }

    # Process-wide mitigations: neither Control Flow Guard nor CET shadow-stack
    # compatibility (docs/architecture.md, mitigation decision). A deliberate
    # change edits that decision, build/Common.props and this audit together.
    $characteristics = @(Get-HeaderField $Headers 'DLL characteristics')
    $guardCf = 0x4000 # IMAGE_DLLCHARACTERISTICS_GUARD_CF
    if ($characteristics.Count -ne 1) {
        $failures.Add('Cannot read the DLL characteristics.')
    } elseif ($characteristics[0] -band $guardCf) {
        $failures.Add(('Release enables Control Flow Guard (GUARD_CF, DLL characteristics 0x{0:X}), contrary to the mitigation decision in docs/architecture.md.' -f $characteristics[0]))
    }
    if ($Headers -match 'CET compatible') {
        $failures.Add('Release is marked CET shadow-stack compatible, contrary to the mitigation decision in docs/architecture.md.')
    }

    # Started from removable or network media, Windows copies the image to the
    # paging file first (/SWAPRUN:CD,NET in src/app/wavplay.vcxproj; memory
    # layout in docs/architecture.md), so its page faults never read that media.
    $fileCharacteristics = @(Get-HeaderField $Headers 'characteristics')
    $runFromSwap = 0x0C00 # IMAGE_FILE_REMOVABLE_RUN_FROM_SWAP | IMAGE_FILE_NET_RUN_FROM_SWAP
    if ($fileCharacteristics.Count -ne 1) {
        $failures.Add('Cannot read the file characteristics.')
    } elseif (($fileCharacteristics[0] -band $runFromSwap) -ne $runFromSwap) {
        $failures.Add(('Release is not marked to run from the paging file on removable and network media (/SWAPRUN:CD,NET, file characteristics 0x{0:X}), contrary to the memory layout in docs/architecture.md.' -f $fileCharacteristics[0]))
    }

    # The debug directory (build/Common.props): whole-program optimization (/GL and
    # /LTCG; docs/architecture.md, optimization policy) leaves an LTCG COFF group,
    # /Brepro a repro entry, and /PDBALTPATH:%_PDB% only the PDB's file name in the
    # CodeView entry (reproducibility in docs/test-specification.md). The hashes
    # that verify.ps1 prints cannot show that either switch was dropped.
    $entries = @($Headers | Where-Object { $_ -cmatch '^\s+[0-9A-F]{8} (?:cv|coffgrp|repro)\s' })
    if (@($entries -cmatch '^\s+[0-9A-F]{8} coffgrp\s.*\s\(LTCG\)\s*$').Count -ne 1) {
        $failures.Add('Release is not built with whole-program optimization: no LTCG COFF group in the debug directory (/GL and /LTCG in build/Common.props; docs/architecture.md, optimization policy).')
    }
    $codeView = @($entries -cmatch '^\s+[0-9A-F]{8} cv\s')
    if ($codeView.Count -ne 1 -or $codeView[0] -cnotmatch 'Format: RSDS, \{[^}]+\}, \d+, wavplay\.pdb\s*$') {
        $failures.Add("The debug directory does not name the PDB by its file name alone (/PDBALTPATH:%_PDB% in build/Common.props; reproducibility in docs/test-specification.md): $($codeView -join ' | ')")
    }
    if (@($entries -cmatch '^\s+[0-9A-F]{8} repro\s').Count -ne 1) {
        $failures.Add('Release has no repro entry in the debug directory (/Brepro in build/Common.props; reproducibility in docs/test-specification.md).')
    }

    # Few, controlled runtime dependencies: the exact set of DLLs the executable
    # imports (including delay loads), compared case-insensitively. The list has
    # no C/C++ runtime DLL, so it also enforces the static CRT. Adding or
    # dropping a DLL is a reviewed edit of this one list.
    $allowedImports = @('KERNEL32', 'USER32', 'GDI32', 'ADVAPI32', 'ole32', 'COMCTL32', 'IMM32')
    # Names not ending in .dll (WINSPOOL.DRV) keep their extension, so they
    # cannot match the list.
    $imports = @($Dependents | Where-Object { Test-ImportLine $_ } | ForEach-Object { $_.Trim() -replace '\.dll$' })
    if (-not $imports.Count) {
        $failures.Add('Cannot read the imported DLLs.')
    } else {
        $unexpected = @($imports | Where-Object { $_ -notin $allowedImports })
        if ($unexpected) {
            $failures.Add("Release imports DLLs outside the allow-list in scripts/verification/release-image.ps1: $($unexpected -join ', ')")
        }
        $unused = @($allowedImports | Where-Object { $_ -notin $imports })
        if ($unused) {
            $failures.Add("Release no longer imports these allow-listed DLLs; remove them from the list in scripts/verification/release-image.ps1: $($unused -join ', ')")
        }
    }
    $failures
}

# Audits the Release executable's headers, then checks that the audit rejects each
# violation introduced into a copy of the real dumps.
function Assert-ReleaseImage {
    param([string]$Executable, [string]$Dumpbin)
    $headers = @(Get-CheckedOutput $Dumpbin @('/nologo', '/headers', $Executable))
    $dependents = @(Get-CheckedOutput $Dumpbin @('/nologo', '/dependents', $Executable))
    $failures = @(Test-ReleaseHeaders $headers $dependents)
    if ($failures.Count) {
        throw "Release image audit of $Executable failed:`n$($failures -join "`n")"
    }

    $lastImport = @(0..($dependents.Count - 1) | Where-Object { Test-ImportLine $dependents[$_] })[-1]
    $variants = @(
        @{ Case = 'a console subsystem'; Expect = 'x64 Windows GUI'
            Headers = $headers -creplace '\b2 subsystem \(Windows GUI\)', '3 subsystem (Windows CUI)' }
        @{ Case = 'the GUARD_CF flag'; Expect = 'Control Flow Guard'; Headers = Set-HeaderFieldBits $headers 'DLL characteristics' 0x4000 0 }
        @{ Case = 'a CET compatibility line'; Expect = 'CET shadow-stack'; Headers = $headers + '                   CET compatible' }
        @{ Case = 'no run from swap on network media'; Expect = 'paging file'; Headers = Set-HeaderFieldBits $headers 'characteristics' 0 0x0800 }
        @{ Case = 'a COFF group without LTCG'; Expect = 'whole-program optimization'
            Headers = $headers -creplace '^(\s+[0-9A-F]{8} coffgrp(?:\s+[0-9A-F]+){3}).*$', '$1' }
        @{ Case = 'a PDB path'; Expect = 'file name alone'
            Headers = $headers -creplace ', wavplay\.pdb\s*$', ', C:\build\out\x64\Release\wavplay.pdb' }
        @{ Case = 'no repro entry'; Expect = '/Brepro'; Headers = @($headers -cnotmatch '^\s+[0-9A-F]{8} repro\s') }
        @{ Case = 'an extra DLL'; Expect = 'outside the allow-list'; Dependents = $dependents + '    WINMM.dll' }
        @{ Case = 'a dropped DLL'; Expect = 'no longer imports'
            Dependents = @(0..($dependents.Count - 1) | Where-Object { $_ -ne $lastImport } | ForEach-Object { $dependents[$_] }) }
    )
    foreach ($variant in $variants) {
        $altered = if ($variant.ContainsKey('Headers')) { $variant.Headers } else { $headers }
        $alteredDependents = if ($variant.ContainsKey('Dependents')) { $variant.Dependents } else { $dependents }
        Assert-Rejected 'Release image audit' $variant.Case @(Test-ReleaseHeaders $altered $alteredDependents) $variant.Expect
    }
    Write-Output "Release image audit self-check: $($variants.Count) altered dumps rejected"
}
