using System.Globalization;
using System.Text;

namespace RtxStudio;

static class HighlightShoulder {
    // The job directory is also the encoder's working directory. A relative path
    // keeps user directory names out of NVEncC's comma-separated filter options.
    internal const string Filter = "matrix=bt709:bt709,transfer=bt709:bt709,colorprim=bt709:bt709,range=limited:limited,lut3d=highlights.cube,lut3d_interp=tetrahedral";

    internal static void WriteLut(string directory) {
        const int size=33;
        const double knee=0.22, shoulderWidth=0.45, residualSlope=0.18;
        using var writer=new StreamWriter(Path.Combine(directory,"highlights.cube"),false,Encoding.ASCII);
        writer.WriteLine($"LUT_3D_SIZE {size}");
        var linear=new double[size];
        for(int i=0;i<size;i++) linear[i]=Math.Pow((double)i/(size-1),2.4);
        for(int b=0;b<size;b++)
        for(int g=0;g<size;g++)
        for(int r=0;r<size;r++) {
            double y=0.2126*linear[r]+0.7152*linear[g]+0.0722*linear[b];
            double gain=1;
            if(y>knee) {
                double delta=y-knee;
                // Unit slope at the knee and a positive slope at white preserve highlight separation.
                double mapped=knee+delta*(residualSlope+(1-residualSlope)/(1+delta/shoulderWidth));
                gain=Math.Pow(mapped/y,1/2.4);
            }
            // A shared gain preserves linear RGB ratios, including on near-neutral colors.
            double scale=gain/(size-1);
            writer.WriteLine(string.Create(CultureInfo.InvariantCulture,$"{r*scale:F7} {g*scale:F7} {b*scale:F7}"));
        }
    }
}
