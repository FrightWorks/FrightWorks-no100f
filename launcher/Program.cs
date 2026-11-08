using Avalonia;

namespace FrightWorks;

static class Program
{
    [STAThread]
    static int Main(string[] args)
    {
        // Velopack (the installed game): during install, update and uninstall it starts this exe with its own arguments,
        // and Run() handles them and exits. Nothing to do there: the player's files live outside the install folder, so
        // an uninstall leaves them (DataPaths). In the zip and the dev tree this does nothing.
        Velopack.VelopackApp.Build().Run();

        // Headless: "--extract <image> <outdir>" writes <outdir>/extract.log; exit 0 ok, 2 holes, 3 wrong disc, 1 error.
        if (args.Length == 3 && args[0] == "--extract")
        {
            var log = new List<string>();
            try
            {
                using var disc = GcDisc.Open(args[1]);
                log.Add($"game {disc.GameId} rev {disc.Revision} \"{disc.Title}\" problem=\"{disc.Problem()}\"");
                if (disc.Problem().Length > 0) { Write(args[2], log); return 3; }
                int n = disc.Extract(args[2], (done, total, name) => { if (done % 500 == 0) log.Add($"{done}/{total} {name}"); });
                log.Add($"extracted {n} files, holes {disc.Holes}");
                Write(args[2], log);
                return disc.Holes == 0 ? 0 : 2;
            }
            catch (Exception e)
            {
                log.Add("ERROR " + e);
                Write(args[2], log);
                return 1;
            }
        }
        // Errors on other threads (a timer, the thread pool, a native callback) would end the process with no message. The
        // runtime stops the process when this handler returns, so show the dialog (Quit only) and wait for the answer; on the
        // UI thread's own death the dialog can't be shown, so the error goes to stderr. UI thread errors that handlers and
        // posted jobs leave uncaught are App's (Dispatcher.UnhandledException), unawaited tasks Fault.Watch's.
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
        {
            if (e.ExceptionObject is not Exception ex) { Console.Error.WriteLine(e.ExceptionObject); return; }
            Console.Error.WriteLine(ex);
            if (Avalonia.Threading.Dispatcher.UIThread.CheckAccess() || Avalonia.Application.Current == null) return;
            try { Fault.Report(ex, canContinue: false).Wait(TimeSpan.FromMinutes(10)); } catch (Exception) { }
        };
        // A faulted task nobody awaited: only noted (it doesn't end the process), the ones we start go through Fault.Watch.
        TaskScheduler.UnobservedTaskException += (_, e) => { Console.Error.WriteLine(e.Exception); e.SetObserved(); };
        return BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
    }

    // Also used by the layout tests and Avalonia's designer.
    public static AppBuilder BuildAvaloniaApp() => AppBuilder.Configure<App>().UsePlatformDetect().WithInterFont();

    static void Write(string dir, List<string> log)
    {
        Directory.CreateDirectory(dir);
        File.WriteAllLines(Path.Combine(dir, "extract.log"), log);
    }
}
