using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.Win32;
using static RtxStudio.Language;

namespace RtxStudio;

sealed record ExportQuality(string Id, string EnglishName, string ChineseName, double Cq, int ReferenceMbps) {
    public static readonly ExportQuality Efficient = new("efficient","Efficient","高效",29,20);
    public static readonly ExportQuality Standard = new("standard","Standard","标准",24,40);
    public static readonly ExportQuality High = new("high","High","高质量",19,80);
    public static readonly ExportQuality[] All = [Efficient,Standard,High];
    public string Label => T(EnglishName,ChineseName);
    public override string ToString() => Chinese ? $"{Label}（约 {ReferenceMbps} Mbps）" : $"{Label} (~{ReferenceMbps} Mbps)";
}
record Options(bool Vsr, bool Hdr, bool Fg, bool Preview, double Start, bool Nvidia, ExportQuality Quality);
record Media(int W, int H, string Rate, double Fps, double Duration, string Transfer, string Primaries,
             string Matrix, string Pixel, string Audio, string Sar, int Rotation, long Frames, double AudioOffset,
             string Range, double VideoMbps);
record Result(string Path, Media Media);

static class Engine {
    internal static readonly string Bin = Path.Combine(AppContext.BaseDirectory, "bin");
    static string F(double value) => value.ToString("0.########", CultureInfo.InvariantCulture);
    static double Number(string value, double fallback = 0) => double.TryParse(value, CultureInfo.InvariantCulture, out var n) ? n : fallback;
    static double Fraction(string value) {
        var p = value.Split('/');
        return p.Length == 2 && Number(p[1]) > 0 ? Number(p[0])/Number(p[1]) : Number(value);
    }
    static string Field(JsonElement e, string key, string fallback = "") =>
        e.ValueKind == JsonValueKind.Object && e.TryGetProperty(key, out var v) ? v.ToString() : fallback;

    static Process Spawn(string name, IEnumerable<string> args, string folder, bool input = false, bool output = false) {
        var info = new ProcessStartInfo(Path.Combine(Bin,name+".exe")) {
            UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = folder,
            RedirectStandardInput = input, RedirectStandardOutput = output, RedirectStandardError = true,
            StandardErrorEncoding = Encoding.UTF8
        };
        foreach (var arg in args) info.ArgumentList.Add(arg);
        info.Environment["XDG_CACHE_HOME"] = folder;
        if (name.StartsWith("vfg/",StringComparison.Ordinal)) {
            var runtime = Path.Combine(Bin,"vfg");
            info.Environment["NV_VIDEO_EFFECTS_PATH"] = runtime;
            info.Environment["PATH"] = runtime + ";" + info.Environment["PATH"];
        }
        return Process.Start(info) ?? throw new IOException("无法启动 " + name);
    }
    static void Kill(Process p) { try { if (!p.HasExited) p.Kill(true); } catch (InvalidOperationException) {} }
    internal static async Task<Media> Probe(string path, CancellationToken token) {
        using var p = Spawn("ffprobe", ["-v","error","-show_streams","-show_format","-of","json",path], Bin, output:true);
        using var reg = token.Register(()=>Kill(p));
        var err = p.StandardError.ReadToEndAsync();
        var data = await p.StandardOutput.ReadToEndAsync();
        await p.WaitForExitAsync(token);
        if (p.ExitCode != 0) throw new IOException(T("Could not read the media file. ","读取媒体失败 ") + await err);
        using var doc = JsonDocument.Parse(data);
        var streams = doc.RootElement.GetProperty("streams").EnumerateArray().ToArray();
        var v = streams.FirstOrDefault(s=>Field(s,"codec_type")=="video" && Field(s.GetProperty("disposition"),"attached_pic")!="1");
        if (v.ValueKind == JsonValueKind.Undefined) throw new IOException(T("This file has no video track.","文件没有视频轨道"));
        var audio = streams.FirstOrDefault(s=>Field(s,"codec_type")=="audio");
        var rate = Field(v,"r_frame_rate");
        if (Fraction(rate) is < 1 or > 240) rate = Field(v,"avg_frame_rate");
        double duration = Number(Field(v,"duration"), Number(Field(doc.RootElement.GetProperty("format"),"duration")));
        int rotation = 0;
        if (v.TryGetProperty("side_data_list",out var side))
            foreach (var s in side.EnumerateArray()) rotation=(int)Number(Field(s,"rotation"),rotation);
        return new((int)Number(Field(v,"width")), (int)Number(Field(v,"height")), rate, Fraction(rate), duration,
            Field(v,"color_transfer"),Field(v,"color_primaries"),Field(v,"color_space"),Field(v,"pix_fmt"),Field(audio,"codec_name"),
            Field(v,"sample_aspect_ratio","1:1"),rotation,(long)Number(Field(v,"nb_frames")),
            Number(Field(audio,"start_time"))-Number(Field(v,"start_time")),Field(v,"color_range"),Number(Field(v,"bit_rate"))/1_000_000);
    }
    static string LosslessDll() {
        var steam = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam")?.GetValue("SteamPath") as string;
        var paths = new List<string>();
        if (steam != null) {
            paths.Add(steam);
            var libraries=Path.Combine(steam,"steamapps","libraryfolders.vdf");
            if (File.Exists(libraries)) foreach (Match m in Regex.Matches(File.ReadAllText(libraries),@"""path""\s+""([^""]+)"""))
                paths.Add(m.Groups[1].Value.Replace(@"\\",@"\"));
        }
        foreach (var root in paths.Distinct()) {
            var path=Path.Combine(root,"steamapps","common","Lossless Scaling","Lossless.dll");
            if (File.Exists(path)) return path;
        }
        throw new IOException(T("Lossless Scaling was not found in Steam. Choose NVIDIA VFG or turn off interpolation.","未找到 Steam 安装的 Lossless Scaling。可取消勾选 LSFG。"));
    }

    static async Task LogLines(StreamReader reader, Action<string> log) {
        while (await reader.ReadLineAsync() is string line) log(line);
    }
    static async Task Run(string exe, IEnumerable<string> args, string cwd, Action<string> log, CancellationToken token) {
        using var p = Spawn(exe,args,cwd,output:true);
        using var stop = token.Register(()=>Kill(p));
        var io = Task.WhenAll(LogLines(p.StandardError,log), LogLines(p.StandardOutput,log));
        await Task.WhenAll(io,p.WaitForExitAsync(token));
        token.ThrowIfCancellationRequested();
        if (p.ExitCode!=0) throw new IOException(exe+T(" exited with code "," 退出，代码 ")+p.ExitCode);
    }
    static async Task Pump(Process left, Process right) {
        try { await left.StandardOutput.BaseStream.CopyToAsync(right.StandardInput.BaseStream, 1024*1024); }
        finally { right.StandardInput.Close(); }
    }
    static async Task Prepare(string input, Media m, Options o, string cwd, string rate, double length,
                              Action<string> log, CancellationToken token) {
        bool finalEncode=!o.Hdr && !o.Vsr;
        if (o.Fg && o.Nvidia && m.Matrix=="bt709" && m.Transfer=="bt709" && m.Primaries=="bt709" &&
            m.Range=="tv" && m.Pixel=="yuv420p" && File.Exists(Path.Combine(Bin,"vfg","vfg-gpu.exe"))) {
            var fastFile=Path.Combine(cwd,"stage.gpu.mp4");
            try {
                log("VFG_GPU_START");
                var gpuArgs=new List<string>{input,fastFile,F(o.Preview?o.Start:0),F(o.Preview?length:-1),m.Rate};
                if(finalEncode) gpuArgs.Add(F(o.Quality.Cq));
                await Run("vfg/vfg-gpu",gpuArgs,cwd,log,token);
                var stage=await Probe(fastFile,token);
                if (stage.Frames<=0 || stage.W!=m.W || stage.H!=m.H || Math.Abs(stage.Fps-2*m.Fps)>.01 ||
                    Math.Abs(stage.Duration-length)>1.1/m.Fps || (!o.Preview && m.Frames>0 && stage.Frames!=2*m.Frames))
                    throw new IOException("GPU output timing mismatch");
                File.Move(fastFile,Path.Combine(cwd,"stage.mp4"));
                return;
            } catch (Exception ex) when (!token.IsCancellationRequested &&
                    (ex is IOException || ex is System.ComponentModel.Win32Exception)) {
                log("VFG_GPU_FALLBACK "+ex.Message);
                File.Delete(fastFile);
            }
        }
        var from = new List<string>{"-nostdin","-v","error"};
        if (o.Preview) from.AddRange(["-ss",F(o.Start)]);
        from.AddRange(["-i",input,"-map","0:V:0","-an","-sn","-dn"]);
        if (o.Preview) from.AddRange(["-t",F(length)]);
        var fps = "fps="+m.Rate;
        const string tags="setparams=range=limited:color_primaries=bt709:color_trc=bt709:colorspace=bt709";
        var normal = "scale=in_color_matrix=auto:out_color_matrix=bt709:out_range=tv,setsar=1,"+tags;
        var enc = new List<string>{"-c:v","hevc_nvenc"};
        if(finalEncode) enc.AddRange(["-preset","p5","-rc","vbr","-cq",F(o.Quality.Cq),"-b:v","0","-maxrate","240M"]);
        else enc.AddRange(["-preset","p3","-rc","constqp","-qp","8"]);
        enc.AddRange(["-bf","0","-pix_fmt","yuv420p","-colorspace","bt709","-color_primaries","bt709","-color_trc","bt709","-color_range","tv","stage.mp4"]);
        if (!o.Fg) {
            from.AddRange(["-vf",fps+","+normal]); from.AddRange(enc);
            await Run("ffmpeg",from,cwd,log,token); return;
        }
        var workerArgs = new List<string>{m.W.ToString(),m.H.ToString()};
        if (!o.Nvidia) workerArgs.Add(LosslessDll());
        from.AddRange(["-vf",fps+",scale=in_color_matrix=auto:out_range=pc,format=rgba","-f","rawvideo","pipe:1"]);
        using var decode = Spawn("ffmpeg",from,cwd,output:true);
        var processes = new List<Process>{decode};
        using var group = CancellationTokenSource.CreateLinkedTokenSource(token);
        try {
            using var worker = Spawn(o.Nvidia ? "vfg/nvidia-vfg" : "lsfg-file",workerArgs,cwd,input:true,output:true);
            processes.Add(worker);
            var args = new List<string>{"-v","error","-f","rawvideo","-pix_fmt","rgba","-video_size",$"{m.W}x{m.H}",
                "-framerate",rate,"-i","pipe:0","-vf","scale=in_range=pc:out_range=tv:out_color_matrix=bt709,format=yuv420p,"+tags};
            args.AddRange(enc);
            using var encode = Spawn("ffmpeg",args,cwd,input:true);
            processes.Add(encode);
            using var cancel=group.Token.Register(()=>{foreach(var p in processes) Kill(p);});
            async Task Guard(Task task) { try {await task;} catch {group.Cancel();throw;} }
            async Task Watch(Process p) {
                await p.WaitForExitAsync();
                if (p.ExitCode!=0) throw new IOException(Path.GetFileName(p.StartInfo.FileName)+T(" exited with code "," 退出，代码 ")+p.ExitCode);
            }
            var jobs = processes.Select(p=>Guard(Watch(p))).Concat(processes.Select(p=>LogLines(p.StandardError,log))).ToList();
            jobs.Add(Guard(Pump(decode,worker))); jobs.Add(Guard(Pump(worker,encode)));
            await Task.WhenAll(jobs);
        } finally { foreach (var p in processes) {try {Kill(p);} catch(ObjectDisposedException){}} token.ThrowIfCancellationRequested(); }
    }

    internal static async Task<Result> Render(string input, string destination, Options o,
        Action<string, double> update, CancellationToken token) {
        var m = await Probe(input,token);
        if (!(o.Vsr || o.Hdr || o.Fg)) throw new ArgumentException(T("Select at least one effect.","请至少选择一种处理效果"));
        if (m.Transfer is "smpte2084" or "arib-std-b67" || m.Primaries is "bt2020" ||
            m.Rotation%360!=0 || m.Sar is not ("1:1" or "N/A" or "") || (m.W|m.H)%2!=0)
            throw new ArgumentException(T("Choose an SDR video with even dimensions, square pixels and no rotation metadata.","此版本支持偶数尺寸、方形像素的横向或纵向 SDR 视频，不支持旋转标记及 HDR 输入。"));
        if (m.Fps is < 1 or > 120 || m.Duration<=0) throw new ArgumentException(T("Could not determine a supported frame rate or duration.","无法确定可用的帧率或时长"));
        var length=o.Preview?Math.Min(5,m.Duration-o.Start):m.Duration;
        if(length<=0 || o.Start<0) throw new ArgumentException(T("The preview start time is outside the video.","预览起点超出视频范围"));
        var fps=m.Fps*(o.Fg?2:1);
        var parts=m.Rate.Split('/');
        var num=long.Parse(parts[0])*(o.Fg?2:1);
        var den=parts.Length>1?long.Parse(parts[1]):1;
        var rate=$"{num}/{den}";
        var scale=o.Vsr?Math.Min(3840.0/m.W,2160.0/m.H):1;
        var width=(int)Math.Round(m.W*scale/2)*2; var height=(int)Math.Round(m.H*scale/2)*2;
        if(o.Vsr && scale<1) throw new ArgumentException(T("This video exceeds 4K. Turn off upscaling.","输入已超过 4K 范围，请关闭超分。"));
        var label=(o.Preview?T("_preview","_预览"):"")+$"_{width}x{height}_{F(Math.Round(fps,3))}fps"+(o.Hdr?"_HDR10":"")+(o.Fg?(o.Nvidia?"_VFG":"_LSFG"):"");
        Directory.CreateDirectory(destination);
        var cwd=Directory.CreateDirectory(Path.Combine(destination,".rtx-"+Guid.NewGuid().ToString("N"))).FullName;
        var logPath=Path.Combine(cwd,"job.log");
        using var writer=new StreamWriter(new FileStream(logPath,FileMode.CreateNew,FileAccess.Write,FileShare.ReadWrite|FileShare.Delete),new UTF8Encoding(false)){AutoFlush=true};
        var stage=0;
        var fgName=o.Nvidia?"NVIDIA VFG High":"LSFG";
        void Log(string line) {
            lock(writer) writer.WriteLine(line);
            if(line=="VFG_GPU_START") fgName=T("NVIDIA VFG High · GPU-memory path","NVIDIA VFG High · 显存直通");
            if(line.StartsWith("VFG_GPU_FALLBACK")) fgName=T("NVIDIA VFG High · compatibility mode","NVIDIA VFG High · 兼容模式");
            if((line.StartsWith("LSFG_FRAMES ") || line.StartsWith("VFG_FRAMES ")) && long.TryParse(line.AsSpan(line.IndexOf(' ')+1),out var count))
                update(fgName+T(" · generating intermediate frames"," 正在生成中间帧"),Math.Min(.47,.47*count/(m.Fps*length)));
            if(stage==1 && Regex.Match(line,@"\]\s*(\d+) frames:") is {Success:true} hit)
                update(T("Encoding · ","正在编码 · ")+o.Quality.Label,.48+.48*Math.Min(1,Number(hit.Groups[1].Value)/Math.Max(1,length*fps)));
        }
        try {
            update(o.Fg?T($"Preparing {fgName} interpolation",$"正在准备 {fgName} 插帧"):T("Decoding source video","正在解码原片"),0);
            await Prepare(input,m,o,cwd,rate,length,Log,token);
            var intermediate=await Probe(Path.Combine(cwd,"stage.mp4"),token);
            var video="stage.mp4";
            bool nativeSizeVsr=o.Vsr && width==m.W && height==m.H;
            if(nativeSizeVsr) {
                // NVEncC skips resize filters at 1:1. Apply RTX VSR explicitly and use its NVENC CQ path.
                stage=1;update(T("Native-size RTX VSR · ","原尺寸 RTX VSR · ")+o.Quality.Label,.48);
                video="enhanced.mp4";
                await Run("sdr2hdr_cli",["stage.mp4",video,o.Hdr?"--vsr-hdr":"--vsr","--legacy","--backend","nvenc",
                    "--codec","hevc","--quality",F(o.Quality.Cq),"--no-audio","--output-size",$"{width}x{height}",
                    "--vsr-quality","4","--max-lum","1000"],cwd,Log,token);
            } else if(o.Hdr || o.Vsr) {
                stage=1;update(T("Encoding · ","正在编码 · ")+o.Quality.Label,.48);
                video="enhanced.mp4";
                var args=new List<string>{"--avhw","-i","stage.mp4","-o",video,"-c","hevc","--preset","P5",
                    "--qvbr",F(o.Quality.Cq),"--max-bitrate","240000","--bframes","0","--sar","1:1"};
                if(o.Vsr) args.AddRange(["--output-res",$"{width}x{height}","--vpp-resize","algo=ngx-vsr,vsr-quality=4"]);
                if(o.Hdr) args.AddRange(["--vpp-ngx-truehdr","contrast=100,saturation=100,middlegray=50,maxluminance=1000",
                    "--output-depth","10","--colormatrix","bt2020nc","--colorprim","bt2020","--transfer","smpte2084",
                    "--master-display","G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,50)","--max-cll","1000,500"]);
                else args.AddRange(["--colormatrix","bt709","--colorprim","bt709","--transfer","bt709"]);
                await Run("encode/NVEncC64",args,cwd,Log,token);
            }
            update(T("Muxing audio and verifying the output","正在写入原声并校验成片"),.97);
            var mux=new List<string>{"-nostdin","-v","error","-i",video};
            if(m.Audio!="") {
                var audioSeek=(o.Preview?o.Start:0)-m.AudioOffset;
                if(audioSeek>0) mux.AddRange(["-ss",F(audioSeek)]);
                else if(audioSeek<0) mux.AddRange(["-itsoffset",F(-audioSeek)]);
                mux.AddRange(["-i",input]);
            }
            mux.AddRange(["-map","0:v:0","-c:v","copy","-tag:v","hvc1"]);
            if(m.Audio!="") {
                mux.AddRange(["-map","1:a:0"]);
                if(!o.Preview && m.Audio=="aac" && Math.Abs(m.AudioOffset)<.002) mux.AddRange(["-c:a","copy"]);
                else mux.AddRange(["-c:a","aac","-b:a","320k"]);
            }
            mux.AddRange(["-video_track_timescale",num.ToString()]);
            // The native-size encoder already writes CFR timestamps in display order, including B-frames.
            if(!nativeSizeVsr) mux.AddRange(["-bsf:v",$"setts=pts=N*{den}:dts=N*{den}:duration={den}:time_base=1/{num}"]);
            mux.AddRange(["-t",F(intermediate.Frames/fps),"-movflags","+faststart","final.mp4"]);
            await Run("ffmpeg",mux,cwd,Log,token);
            var final=Path.Combine(cwd,"final.mp4");var info=await Probe(final,token);
            if(info.W!=width || info.H!=height || info.Frames!=intermediate.Frames || info.Frames<1 ||
               Math.Abs(info.Duration-length)>Math.Max(.16,2/m.Fps) || Math.Abs(info.Fps-fps)>.01 ||
               (m.Audio!="" && info.Audio=="") || (o.Hdr && (info.Transfer!="smpte2084" || info.Primaries!="bt2020" || !info.Pixel.Contains("10"))))
                 throw new IOException(T("Output verification failed","输出校验失败")+$" · {info.W}×{info.H} · {info.Frames}/{intermediate.Frames} · {F(info.Duration)} s");
            token.ThrowIfCancellationRequested();
            var stem=Path.GetFileNameWithoutExtension(input)+label;
            for(var i=0;;i++) {
                var path=Path.Combine(destination,stem+(i==0?"":$" ({i})")+".mp4");
                try {File.Move(final,path); return new(path,info);}
                catch(IOException) when(File.Exists(path)) {}
            }
        } catch(Exception e) when(!token.IsCancellationRequested) {
            writer.Flush();
            var diag=Path.Combine(destination,T("RTX-error-","RTX-错误-")+Guid.NewGuid().ToString("N")[..8]+".log");
            File.Copy(logPath,diag);
            throw new IOException(e.Message+Environment.NewLine+T("Log: ","日志：")+diag,e);
        } finally {
            writer.Close();
            for(int attempt=0;attempt<6;attempt++) {
                try {Directory.Delete(cwd,true);break;}
                catch(DirectoryNotFoundException){break;}
                catch(IOException){await Task.Delay(150*(attempt+1));}
            }
        }
    }
}
