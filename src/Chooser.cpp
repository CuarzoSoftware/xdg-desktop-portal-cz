#include <Chooser.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sys/wait.h>
#include <unistd.h>

// ~/.config/xdg-desktop-portal-cz/config: key=value lines
static std::map<std::string, std::string> ReadConfig() noexcept
{
    std::map<std::string, std::string> config;
    const char *xdg { getenv("XDG_CONFIG_HOME") };
    const char *home { getenv("HOME") };
    const std::string dir { xdg && *xdg ? std::string(xdg) : std::string(home ? home : "") + "/.config" };
    std::ifstream file { dir + "/xdg-desktop-portal-cz/config" };
    std::string line;

    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#')
            continue;

        const auto eq { line.find('=') };

        if (eq == std::string::npos)
            continue;

        auto trim = [](std::string s) {
            s.erase(0, s.find_first_not_of(" \t"));
            s.erase(s.find_last_not_of(" \t") + 1);
            return s;
        };

        config[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }

    return config;
}

static bool InPath(const std::string &program) noexcept
{
    const char *path { getenv("PATH") };

    if (!path)
        return false;

    std::string p { path };
    size_t start { 0 };

    while (start <= p.size())
    {
        const size_t end { std::min(p.find(':', start), p.size()) };

        if (access((p.substr(start, end - start) + "/" + program).c_str(), X_OK) == 0)
            return true;

        start = end + 1;
    }

    return false;
}

// Runs @p command with @p input on its stdin, returns the first line of its output ("" if cancelled)
static std::string RunChooser(const std::string &command, const std::string &input) noexcept
{
    int in[2], out[2];

    if (pipe(in) != 0)
        return {};

    if (pipe(out) != 0)
    {
        close(in[0]); close(in[1]);
        return {};
    }

    const pid_t pid { fork() };

    if (pid < 0)
        return {};

    if (pid == 0)
    {
        dup2(in[0], STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
        _exit(127);
    }

    close(in[0]);
    close(out[1]);
    [[maybe_unused]] const auto written { write(in[1], input.data(), input.size()) };
    close(in[1]);

    std::string output;
    char buf[512];
    ssize_t n;

    while ((n = read(out[0], buf, sizeof(buf))) > 0)
        output.append(buf, size_t(n));

    close(out[0]);

    int status;
    waitpid(pid, &status, 0);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return {};

    if (const auto nl { output.find('\n') }; nl != std::string::npos)
        output.resize(nl);

    return output;
}

bool ChooseTarget(UInt32 types, Target &target) noexcept
{
    auto &portal { Portal::Get() };
    const bool monitors { (types & Monitor) && !portal.outputs.empty() };
    const bool windows { (types & Window) && portal.toplevelSourceManager && !portal.toplevels.empty() };

    struct Choice { std::string label; Output *output; Toplevel *toplevel; };
    std::vector<Choice> choices;

    if (monitors)
        for (const auto &o : portal.outputs)
            choices.push_back({ "Monitor: " + o->name + (o->description.empty() ? "" : " " + o->description), o.get(), nullptr });

    if (windows)
        for (const auto &t : portal.toplevels)
            choices.push_back({ "Window: " + t->title + (t->appId.empty() ? "" : " (" + t->appId + ")"), nullptr, t.get() });

    if (choices.empty())
        return false;

    const auto apply = [&target](const Choice &c) {
        target.type = c.output ? Monitor : Window;
        target.output = c.output;
        target.toplevel = c.toplevel;
        return true;
    };

    if (choices.size() == 1)
        return apply(choices[0]);

    const auto config { ReadConfig() };

    // Screens only: an output picker (prints the output name)
    if (monitors && !windows)
    {
        std::string cmd;

        if (const auto it { config.find("output_chooser_cmd") }; it != config.end())
            cmd = it->second;
        else if (InPath("slurp"))
            cmd = "slurp -f %o -or";

        if (!cmd.empty())
        {
            const std::string name { RunChooser(cmd, "") };

            if (name.empty())
                return false;

            if (Output *output { portal.findOutput(name) })
                return apply({ "", output, nullptr });
        }
    }

    // A menu with every choice
    std::string cmd;

    if (const auto it { config.find("chooser_cmd") }; it != config.end())
        cmd = it->second;
    else if (InPath("wofi"))
        cmd = "wofi -d -n --prompt='Select what to share'";
    else if (InPath("fuzzel"))
        cmd = "fuzzel --dmenu --prompt='Share: '";
    else if (InPath("bemenu"))
        cmd = "bemenu -p 'Share:'";
    else if (InPath("wmenu"))
        cmd = "wmenu -p 'Share:'";

    if (cmd.empty())
    {
        // No way to ask: the first screen (the compositor may still ask the user)
        Log(Info, "No chooser available: sharing the first choice");
        return apply(choices[0]);
    }

    std::string input;

    for (const auto &c : choices)
        input += c.label + "\n";

    const std::string chosen { RunChooser(cmd, input) };

    for (const auto &c : choices)
        if (c.label == chosen)
            return apply(c);

    return false;
}
