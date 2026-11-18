using System.Diagnostics;
using System.Globalization;
using static RtxStudio.Language;

namespace RtxStudio;

static class Program {
    [STAThread]
    static int Main(string[] args) {
        Language.Load(Language.SettingsPath);
        if(args.Length>=3 && args[0]=="--render") {
            var opts=args.Skip(3).ToArray();
            var offset=Array.IndexOf(opts,"--preview");
            try {
                var start=offset>=0?double.Parse(opts[offset+1],CultureInfo.InvariantCulture):0;
                var q=Array.IndexOf(opts,"--quality");
                var quality=q<0 ? ExportQuality.Standard : ExportQuality.All.FirstOrDefault(p=>
                    string.Equals(p.Id,opts.ElementAtOrDefault(q+1),StringComparison.OrdinalIgnoreCase))
                    ?? throw new ArgumentException(T("--quality requires efficient, standard or high.","--quality 的可选值为 efficient、standard、high。"));
                var h=Array.IndexOf(opts,"--hdr-look");
                var look=h<0 ? HdrLook.Gentle : HdrLook.All.FirstOrDefault(p=>
                    string.Equals(p.Id,opts.ElementAtOrDefault(h+1),StringComparison.OrdinalIgnoreCase))
                    ?? throw new ArgumentException(T("--hdr-look requires gentle or classic.","--hdr-look 的可选值为 gentle、classic。"));
                var options=new Options(!opts.Contains("--no-vsr"),!opts.Contains("--no-hdr"),!opts.Contains("--no-fg"),offset>=0,start,opts.Contains("--nvidia-vfg"),quality,look);
                var result=Engine.Render(Path.GetFullPath(args[1]),Path.GetFullPath(args[2]),options,
                    (message,n)=>Console.WriteLine($"{n:P0} {message}"),CancellationToken.None).GetAwaiter().GetResult();
                Console.WriteLine(result.Path);return 0;
            } catch(Exception ex) {Console.Error.WriteLine(ex.Message);return 1;}
        }
        ApplicationConfiguration.Initialize();
        Application.Run(new StudioForm(args.FirstOrDefault(),Language.SettingsPath));
        return 0;
    }
}

sealed class StudioForm : Form {
    readonly Label source=new(),details=new(),status=new(),target=new(),folder=new();
    readonly CheckBox vsr=new(){Checked=true,AutoSize=true};
    readonly CheckBox hdr=new(){Text="RTX Video HDR",Checked=true,AutoSize=true};
    readonly CheckBox fg=new(){Checked=true,AutoSize=true};
    readonly ComboBox method=new(){DropDownStyle=ComboBoxStyle.DropDownList,Width=167};
    readonly ComboBox quality=new(){DropDownStyle=ComboBoxStyle.DropDownList};
    readonly ComboBox hdrLook=new(){DropDownStyle=ComboBoxStyle.DropDownList};
    readonly Label lookLabel=new(){AutoSize=true},lookHint=new(){AutoSize=true,ForeColor=Color.DimGray};
    readonly NumericUpDown seconds=new(){Maximum=86400,Width=86};
    readonly ProgressBar progress=new(){Dock=DockStyle.Fill,Maximum=1000};
    readonly Button choose=new(),location=new(),
        test=new(),render=new(),cancel=new(){Enabled=false},
        open=new(){Enabled=false};
    CancellationTokenSource? running;
    string? input,outputFile,outputDir;
    Media? media;
    int selection;
    bool closeWhenDone;

    readonly ComboBox language=new(){DropDownStyle=ComboBoxStyle.DropDownList,AccessibleName="Language / 语言",Dock=DockStyle.Top};
    readonly Label title=new(), qualityLabel=new(){AutoSize=true}, qualityHint=new(){AutoSize=true}, previewLabel=new(){AutoSize=true};
    string? selectedPath;
    Func<string>? lastStatus;

    public StudioForm(string? initial,string settingsPath) {
        Text="RTX Video Studio";Font=new Font("Microsoft YaHei UI",10);
        AutoScaleMode=AutoScaleMode.None;StartPosition=FormStartPosition.CenterScreen;
        ClientSize=new Size(700,488);MinimumSize=new Size(640,490);
        BackColor=Color.FromArgb(247,248,250);AllowDrop=true;
        var layout=new TableLayoutPanel{Dock=DockStyle.Fill,Padding=new Padding(22,16,22,16),ColumnCount=2,RowCount=12};
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));layout.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute,110));
        foreach(var h in new[]{40,32,26,38,40,40,42,34,40,50,22,34})layout.RowStyles.Add(new RowStyle(SizeType.Absolute,h));
        Controls.Add(layout);
        void FitDpi() {
            int Px(int logical) => (int)Math.Round(logical*DeviceDpi/96.0);
            MinimumSize = new Size(Px(810),Px(572));
            ClientSize = new Size(Px(850),Px(532));
            layout.Padding = new Padding(Px(22),Px(16),Px(22),Px(16));
            layout.ColumnStyles[1].Width=Px(150);
            int[] heights=[43,38,30,47,40,40,35,40,45,48,20,39];
            for(int i=0;i<heights.Length;i++) layout.RowStyles[i].Height=Px(heights[i]);
            method.Width=Px(180);quality.Width=hdrLook.Width=Px(195);seconds.Width=Px(80);
            foreach(var panel in layout.Controls.OfType<FlowLayoutPanel>())
                foreach(Control child in panel.Controls) child.Margin=new Padding(0,Px(4),Px(13),0);
            foreach(var b in new[]{choose,location,test,render,open,cancel}) b.MinimumSize=new Size(Px(65),Px(28));
        }
        Load+=(_,_)=>FitDpi();
        DpiChanged+=(_,_)=>BeginInvoke(new Action(FitDpi));
        void Row(Control c,int row,int span=2) {layout.Controls.Add(c,0,row);layout.SetColumnSpan(c,span);}
        void LabelStyle(Label l) {l.Dock=DockStyle.Fill;l.AutoEllipsis=true;l.TextAlign=ContentAlignment.MiddleLeft;}
        foreach(var l in new[]{source,details,status,target,folder})LabelStyle(l);
        title.Font=new Font(Font.FontFamily,18,FontStyle.Bold);LabelStyle(title);Row(title,0,1);
        language.Items.AddRange(["English","简体中文"]);language.SelectedIndex=Chinese?1:0;
        layout.Controls.Add(language,1,0);
        Row(source,1,1);layout.Controls.Add(choose,1,1);
        details.ForeColor=Color.DimGray;Row(details,2);
        FlowLayoutPanel Flow(params Control[] controls) {
            var f=new FlowLayoutPanel{Dock=DockStyle.Fill,WrapContents=false,FlowDirection=FlowDirection.LeftToRight};
            foreach(var c in controls){c.Margin=new Padding(0,4,15,0);f.Controls.Add(c);}return f;
        }

        fg.CheckedChanged+=(_,_)=>method.Enabled=fg.Checked;
        Row(Flow(vsr,hdr,fg,method),3);
        lookLabel.Padding=lookHint.Padding=qualityLabel.Padding=qualityHint.Padding=previewLabel.Padding=new Padding(0,4,0,0);
        qualityHint.ForeColor=Color.DimGray;
        Row(Flow(lookLabel,hdrLook,lookHint),4);
        hdrLook.SelectedIndexChanged+=(_,_)=>RefreshLook();
        hdr.CheckedChanged+=(_,_)=>RefreshLook();
        Row(Flow(qualityLabel,quality,qualityHint),5);
        target.ForeColor=Color.FromArgb(0,86,102);Row(target,6);
        Row(folder,7,1);layout.Controls.Add(location,1,7);
        Row(Flow(previewLabel,seconds,test),8);
        foreach(var b in new[]{choose,location,test,render,open,cancel}){b.AutoSize=true;b.Height=30;b.FlatStyle=FlatStyle.System;}
        Row(Flow(render,cancel,open),9);Row(progress,10);Row(status,11);
        choose.Click+=async (_,_)=>{using var d=new OpenFileDialog{Title=T("Choose a video","选择视频"),Filter=T("Video","视频")+"|*.mp4;*.mkv;*.mov;*.m4v;*.avi;*.webm;*.ts|"+T("All files","所有文件")+"|*.*"};if(d.ShowDialog()==DialogResult.OK)await SelectFile(d.FileName);};
        location.Click+=(_,_)=>{using var d=new FolderBrowserDialog{InitialDirectory=outputDir??(input==null?"":Path.GetDirectoryName(input)??"")};if(d.ShowDialog()==DialogResult.OK){outputDir=d.SelectedPath;folder.Text=outputDir;}};
        foreach(var c in new[]{vsr,hdr,fg})c.CheckedChanged+=(_,_)=>RefreshTarget();
        render.Click+=async (_,_)=>await ProcessVideo(false);test.Click+=async (_,_)=>await ProcessVideo(true);
        cancel.Click+=(_,_)=>{running?.Cancel();cancel.Enabled=false;status.Text=T("Cancelling…","正在取消…");};
        open.Click+=(_,_)=>{if(File.Exists(outputFile))Process.Start(new ProcessStartInfo(outputFile){UseShellExecute=true});};
        DragEnter+=(_,e)=>e.Effect=running==null && e.Data?.GetDataPresent(DataFormats.FileDrop)==true?DragDropEffects.Copy:DragDropEffects.None;
        DragDrop+=async (_,e)=>{if(running==null && e.Data?.GetData(DataFormats.FileDrop) is string[] files && files.Length>0)await SelectFile(files[0]);};
        FormClosing+=(_,e)=>{if(running!=null){e.Cancel=true;closeWhenDone=true;running.Cancel();}};
        Shown+=async (_,_)=>{if(initial!=null && File.Exists(initial))await SelectFile(initial);};
        language.SelectedIndexChanged+=async (_,_)=>{
            Chinese=language.SelectedIndex==1;
            try { Language.Save(settingsPath); }
            catch(Exception ex) when(ex is IOException or UnauthorizedAccessException) {
                MessageBox.Show(this,T("Could not save your language preference.","无法保存语言偏好。")+Environment.NewLine+ex.Message,Text);
            }
            ApplyLanguage();
            if(selectedPath!=null && media==null) await SelectFile(selectedPath);
        };
        ApplyLanguage();
    }
    void ApplyLanguage() {
        title.Text=T("Video enhancement","视频增强");
        choose.Text=T("Choose video…","选择视频…");location.Text=T("Change folder…","更改目录…");
        test.Text=T("Preview 5 seconds","生成 5 秒预览");render.Text=T("Export full video","处理完整视频");
        cancel.Text=T("Cancel","取消");open.Text=T("Open output","打开成片");
        vsr.Text=T("RTX VSR to 4K","RTX VSR 超分至 4K");fg.Text=T("2× interpolation","2 倍插帧");
        previewLabel.Text=T("Preview start (s)","预览起点（秒）");
        hdrLook.AccessibleName=lookLabel.Text=T("HDR look","HDR 风格");
        var selectedLook=hdrLook.SelectedItem??HdrLook.Gentle;
        hdrLook.Items.Clear();hdrLook.Items.AddRange(HdrLook.All);hdrLook.SelectedItem=selectedLook;
        quality.AccessibleName=qualityLabel.Text=T("Export quality","导出质量");
        qualityHint.Text=T("4K60 estimates. Bitrate varies with footage.","4K60 参考码率，实际随画面变化");
        method.AccessibleName=T("Interpolation engine","插帧引擎");
        int engine=Math.Max(0,method.SelectedIndex);
        method.Items.Clear();method.Items.AddRange(["Lossless Scaling · LSFG",T("NVIDIA VFG · High","NVIDIA VFG · 高质量")]);
        method.SelectedIndex=engine;
        var preset=quality.SelectedItem??ExportQuality.Standard;
        quality.Items.Clear();quality.Items.AddRange(ExportQuality.All);quality.SelectedItem=preset;
        source.Text=selectedPath==null?T("Drop a video here or choose a file","拖入视频，或选择本地文件"):Path.GetFileName(selectedPath);
        folder.Text=outputDir??(input!=null?Path.GetDirectoryName(input):T("Save next to the source video","保存到原视频所在文件夹"));
        if(media!=null) ShowMedia(media);
        status.Text=lastStatus?.Invoke()??"";
        RefreshTarget();
    }
    void ShowMedia(Media m) {
        details.Text=$"{m.W} × {m.H}   ·   {m.Fps:0.###} fps   ·   {m.Duration:0.##} {T("s","秒")}   ·   {m.Transfer}";
    }
    void RefreshLook() {
        hdrLook.Enabled=hdr.Checked && running==null;
        lookHint.Text=!hdr.Checked ? "" : Equals(hdrLook.SelectedItem,HdrLook.Classic)
            ? T("Original RTX mapping · 1,000-nit target","原版 RTX 映射 · 目标 1000 尼特")
            : T("Softer highlights · 500-nit target","柔化浅色和高光 · 目标 500 尼特");
    }
    async Task SelectFile(string path) {
        int revision=++selection;selectedPath=path;lastStatus=null;media=null;input=null;outputFile=null;open.Enabled=false;
        source.Text=Path.GetFileName(path);details.Text=T("Reading video…","正在读取…");status.Text="";progress.Value=0;RefreshTarget();
        try {
            var m=await Engine.Probe(path,CancellationToken.None);
            if(revision!=selection)return;
            media=m;input=Path.GetFullPath(path);
            ShowMedia(m);
            seconds.Maximum=Math.Max(0,(decimal)Math.Ceiling(m.Duration)-1);seconds.Value=0;
            folder.Text=outputDir??Path.GetDirectoryName(input);
        } catch(Exception e){if(revision==selection) details.Text=e.Message;}
        RefreshTarget();
    }
    void RefreshTarget() {
        render.Enabled=test.Enabled=running==null && media!=null && (vsr.Checked||hdr.Checked||fg.Checked);
        if(media==null){target.Text="";return;}
        var m=media;var scale=vsr.Checked?Math.Min(3840.0/m.W,2160.0/m.H):1;
        target.Text=T("Output","输出")+$"   {Math.Round(m.W*scale/2)*2} × {Math.Round(m.H*scale/2)*2}   ·   {m.Fps*(fg.Checked?2:1):0.###} fps   ·   {(hdr.Checked?"HDR10":"SDR")}   ·   MP4";
    }
    async Task ProcessVideo(bool preview) {
        if(input==null || running!=null)return;
        using var job=new CancellationTokenSource();running=job;
        var o=new Options(vsr.Checked,hdr.Checked,fg.Checked,preview,(double)seconds.Value,method.SelectedIndex==1,(ExportQuality)quality.SelectedItem!,(HdrLook)hdrLook.SelectedItem!);
        foreach(var c in new Control[]{choose,location,vsr,hdr,hdrLook,fg,method,quality,language,seconds,test,render,open})c.Enabled=false;
        outputFile=null;cancel.Enabled=true;progress.Value=0;
        var updates=new Progress<(string Text,double Part)>(v=>{if(running==job && !job.IsCancellationRequested){status.Text=v.Text;progress.Value=Math.Clamp((int)(1000*v.Part),0,1000);}});
        try {
            var r=await Engine.Render(input,outputDir??Path.GetDirectoryName(input)!,o,(s,n)=>((IProgress<(string,double)>)updates).Report((s,n)),job.Token);
            outputFile=r.Path;open.Enabled=true;progress.Value=1000;lastStatus=()=>$"{T("Saved","已保存")} · {o.Quality.Label} · {r.Media.VideoMbps:0.#} Mbps · {Path.GetFileName(r.Path)}";status.Text=lastStatus();
        } catch(OperationCanceledException){lastStatus=()=>T("Cancelled","已取消");status.Text=lastStatus();progress.Value=0;}
        catch(Exception e){lastStatus=()=>T("Export failed","处理失败");status.Text=lastStatus();MessageBox.Show(this,e.Message,Text,MessageBoxButtons.OK,MessageBoxIcon.Error);}
        finally {
            running=null;cancel.Enabled=false;
            foreach(var c in new Control[]{choose,location,vsr,hdr,fg,quality,language,seconds})c.Enabled=true;
            method.Enabled=fg.Checked;RefreshLook();RefreshTarget();if(closeWhenDone)Close();
        }
    }
}
