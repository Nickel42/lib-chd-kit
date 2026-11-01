// Example: create, inspect, verify and extract a CHD from C#, with progress
// reporting and cancellation, the way a GUI app would.
// SPDX-License-Identifier: BSD-3-Clause

using System;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using LibChdKit;

var dir = Path.Combine(Path.GetTempPath(), "chdman-csharp-example");
Directory.CreateDirectory(dir);
string image = Path.Combine(dir, "disk.img"), chd = Path.Combine(dir, "disk.chd"), back = Path.Combine(dir, "disk.out");

var bytes = new byte[1 << 20];
for (int i = 0; i < bytes.Length / 2; i++) bytes[i] = (byte)(i * 7);
File.WriteAllBytes(image, bytes);

Console.WriteLine($"lib-chd-kit {Chdman.Version}, ABI {Chdman.AbiVersion}");

using var chdman = new Chdman();
chdman.Progress = new Progress<ChdmanProgress>(p => Console.WriteLine($"  {p.Phase} {p.Fraction:P0}"));
chdman.Log = (level, message) => Console.WriteLine($"  [{level}] {message}");

// operations block, so run them off the UI thread; the token cancels them
using var cts = new CancellationTokenSource();
await Task.Run(() => chdman.CreateRaw(image, chd, unitSize: 512, new ChdOutputSettings { Force = true }, cancellationToken: cts.Token));

var info = chdman.Info(chd);
Console.WriteLine($"created {info.MediaType} CHD: {info.LogicalBytes} -> {info.FileBytes} bytes, {info.Compression}");
Console.WriteLine($"raw SHA-1 {info.RawSha1}");
Console.WriteLine($"verify: {chdman.Verify(chd).Outcome}");

chdman.ExtractRaw(chd, back, force: true);
Console.WriteLine($"round trip identical: {File.ReadAllBytes(back).SequenceEqual(bytes)}");

foreach (var t in Chdman.Templates.Where(t => t.Id % 8 == 0))
    Console.WriteLine($"template {t.Id}: {t.Manufacturer} {t.Model} {t.Cylinders}/{t.Heads}/{t.Sectors}");

try { chdman.CreateRaw(image, chd, 512); }
catch (ChdmanException e) { Console.WriteLine($"expected error -> {e.Status}: {e.Message}"); }
