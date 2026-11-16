// MSBuild RoslynCodeTaskFactory fragment; never compiled into the GUI.
// The framework MSIX already contains the merged, Microsoft-generated index.
// Reuse it verbatim while our programmatic UI has no application XAML/resources.
try
{
    using (var archive = new ZipArchive(File.OpenRead(FrameworkPackage), ZipArchiveMode.Read))
    {
        foreach (var name in new[] { "Microsoft.ui.xaml.dll", "Microsoft.UI.Xaml.Controls.pri", "Microsoft.WindowsAppRuntime.dll", "MRM.dll" })
        {
            var entry = archive.Entries.FirstOrDefault(item => string.Equals(item.FullName, name, StringComparison.OrdinalIgnoreCase));
            var path = Directory.EnumerateFiles(PublishDirectory).FirstOrDefault(file => string.Equals(Path.GetFileName(file), name, StringComparison.OrdinalIgnoreCase));
            if (entry == null || path == null)
                throw new InvalidDataException("Missing runtime component for resource-index validation: " + name);
            using (var incoming = entry.Open())
            using (var expected = new MemoryStream())
            {
                incoming.CopyTo(expected);
                if (!expected.ToArray().SequenceEqual(File.ReadAllBytes(path)))
                    throw new InvalidDataException("Framework PRI/runtime version mismatch: " + name);
            }
        }
        var resources = archive.GetEntry("resources.pri");
        if (resources == null || resources.Length < 1024)
            throw new InvalidDataException("The matching framework MSIX has no merged resources.pri");
        using (var incoming = resources.Open())
        using (var output = File.Create(Path.Combine(PublishDirectory, "resources.pri")))
            incoming.CopyTo(output);
        Log.LogMessage(Microsoft.Build.Framework.MessageImportance.High,
            "Published matching merged WinUI resources.pri ({0} bytes)", resources.Length);
        // Windows App SDK 1.8's self-contained payload omits this signed
        // resource DLL, but notification registration/cleanup load it. Keep it
        // version-bound to the runtime validated above, just like resources.pri.
        const string notificationName = "Microsoft.WindowsAppRuntime.Insights.Resource.dll";
        var notificationResource = archive.GetEntry(notificationName);
        if (notificationResource == null || notificationResource.Length == 0)
            throw new InvalidDataException("The matching framework MSIX has no " + notificationName);
        var notificationPath = Path.Combine(PublishDirectory, notificationName);
        using (var incoming = notificationResource.Open())
        using (var expected = new MemoryStream())
        {
            incoming.CopyTo(expected);
            var bytes = expected.ToArray();
            if (File.Exists(notificationPath) && !bytes.SequenceEqual(File.ReadAllBytes(notificationPath)))
                throw new InvalidDataException("Notification registration resource/runtime version mismatch");
            File.WriteAllBytes(notificationPath, bytes);
        }
        Log.LogMessage(Microsoft.Build.Framework.MessageImportance.High,
            "Published matching notification registration resource DLL ({0} bytes)", notificationResource.Length);
    }
}
catch (Exception error)
{
    Log.LogErrorFromException(error, true);
    return false;
}
