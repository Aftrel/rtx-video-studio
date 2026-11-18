using System.Globalization;

namespace RtxStudio;

static class Language {
    internal static readonly string SettingsPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "RTX Video Studio", "language.txt");
    internal static bool Chinese { get; set; } = SystemIsChinese;
    static bool SystemIsChinese => CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "zh";
    internal static string T(string english,string chinese) => Chinese ? chinese : english;

    internal static void Load(string path) {
        Chinese=SystemIsChinese;
        try {
            Chinese=File.ReadAllText(path).Trim() switch {"zh-CN"=>true,"en"=>false,_=>SystemIsChinese};
        } catch(IOException) {} catch(UnauthorizedAccessException) {}
    }
    internal static void Save(string path) {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temp=path+"."+Guid.NewGuid().ToString("N")+".tmp";
        try {
            File.WriteAllText(temp,Chinese?"zh-CN":"en");
            File.Move(temp,path,true);
        } finally { File.Delete(temp); }
    }
}
