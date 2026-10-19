#include "ClaudeProcess.h"

#include "Paths.h"
#include "Text.h"

#include <windows.h>

#include <format>

namespace aiad::ai
{

namespace
{

// Windows command-line quoting (CommandLineToArgvW rules).
std::wstring Quote(const std::wstring& arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return arg;
    std::wstring out = L"\"";
    for (size_t i = 0;; ++i)
    {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\')
        {
            ++i;
            ++backslashes;
        }
        if (i == arg.size())
        {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"')
        {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
        }
        else
        {
            out.append(backslashes, L'\\');
            out += arg[i];
        }
    }
    return out + L"\"";
}

std::wstring EnvironmentBlock(const std::map<std::string, std::string>& overrides)
{
    std::map<std::wstring, std::wstring, decltype([](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; })> vars;
    wchar_t* block = GetEnvironmentStringsW();
    for (const wchar_t* p = block; *p; p += wcslen(p) + 1)
    {
        const std::wstring entry = p;
        const size_t eq = entry.find(L'=', 1);
        if (eq != std::wstring::npos)
            vars[entry.substr(0, eq)] = entry.substr(eq + 1);
    }
    FreeEnvironmentStringsW(block);
    for (const auto& [k, v] : overrides)
    {
        if (v.empty())
            vars.erase(Widen(k));
        else
            vars[Widen(k)] = Widen(v);
    }
    std::wstring out;
    for (const auto& [k, v] : vars)
        out += k + L"=" + v + L'\0';
    out += L'\0';
    return out;
}

}  // namespace

static std::wstring CommandLineOf(const std::filesystem::path& executable, const std::vector<std::string>& arguments)
{
    std::wstring line = Quote(executable.wstring());
    for (const std::string& a : arguments)
        line += L" " + Quote(Widen(a));
    return line;
}

CapturedRun RunCaptured(const std::filesystem::path& executable, const std::vector<std::string>& arguments, const std::filesystem::path& cwd,
                        int timeoutMs)
{
    CapturedRun run;
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE readEnd, writeEnd, nul;
    if (!CreatePipe(&readEnd, &writeEnd, &inherit, 1 << 20))
        return run;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nul;
    startup.hStdOutput = writeEnd;
    startup.hStdError = writeEnd;
    PROCESS_INFORMATION info{};
    std::wstring line = CommandLineOf(executable, arguments);
    const std::wstring dir = cwd.wstring();
    run.started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.empty() ? nullptr : dir.c_str(), &startup, &info);
    CloseHandle(writeEnd);
    CloseHandle(nul);
    if (run.started)
    {
        std::thread reader([&] {
            char buffer[8192];
            DWORD read = 0;
            while (ReadFile(readEnd, buffer, sizeof(buffer), &read, nullptr) && read > 0)
                run.output.append(buffer, read);
        });
        if (WaitForSingleObject(info.hProcess, DWORD(timeoutMs)) == WAIT_TIMEOUT)
            TerminateProcess(info.hProcess, 1);
        reader.join();
        GetExitCodeProcess(info.hProcess, &run.exitCode);
        CloseHandle(info.hProcess);
        CloseHandle(info.hThread);
    }
    CloseHandle(readEnd);
    return run;
}

bool RunInConsole(const std::filesystem::path& executable, const std::vector<std::string>& arguments, const std::filesystem::path& cwd)
{
    // cmd /k keeps the window open so the user reads the outcome.
    std::wstring line = L"cmd.exe /d /k \"" + CommandLineOf(executable, arguments) + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION info{};
    const std::wstring dir = cwd.wstring();
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, dir.empty() ? nullptr : dir.c_str(), &startup, &info))
        return false;
    CloseHandle(info.hProcess);
    CloseHandle(info.hThread);
    return true;
}

std::filesystem::path ResolveClaude(const std::string& configured)
{
    std::vector<std::filesystem::path> candidates;
    if (!configured.empty())
        candidates.emplace_back(Widen(configured));
    wchar_t found[MAX_PATH];
    for (const wchar_t* name : {L"claude.exe", L"claude.cmd"})
        if (SearchPathW(nullptr, name, nullptr, MAX_PATH, found, nullptr))
            candidates.emplace_back(found);
    candidates.push_back(HomeDir() / ".local" / "bin" / "claude.exe");
    for (const auto& c : candidates)
    {
        std::error_code ec;
        if (std::filesystem::is_regular_file(c, ec))
            return c;
    }
    return {};
}

std::string ClaudeVersion(const std::filesystem::path& executable)
{
    if (executable.empty())
        return {};
    std::string text = RunCaptured(executable, {"--version"}, std::filesystem::temp_directory_path(), 15000).output;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    return text;
}

ClaudeProcess::~ClaudeProcess()
{
    Stop();
}

bool ClaudeProcess::Start(const ProcessOptions& options, std::string& error)
{
    Stop();
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE stdinRead, stdinWrite, stdoutRead, stdoutWrite, stderrRead, stderrWrite;
    if (!CreatePipe(&stdinRead, &stdinWrite, &inherit, 0) || !CreatePipe(&stdoutRead, &stdoutWrite, &inherit, 1 << 20) ||
        !CreatePipe(&stderrRead, &stderrWrite, &inherit, 0))
    {
        error = "CreatePipe: " + LastErrorText();
        return false;
    }
    SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0);

    std::wstring commandLine;
    const std::wstring exe = options.executable.wstring();
    const bool script = options.executable.extension() == ".cmd" || options.executable.extension() == ".bat";
    if (script)
        commandLine = L"cmd.exe /d /s /c \"" + Quote(exe);
    else
        commandLine = Quote(exe);
    for (const std::string& a : options.arguments)
        commandLine += L" " + Quote(Widen(a));
    if (script)
        commandLine += L"\"";

    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdinRead;
    startup.hStdOutput = stdoutWrite;
    startup.hStdError = stderrWrite;
    PROCESS_INFORMATION info{};
    std::wstring environment = EnvironmentBlock(options.environment);
    const std::wstring cwd = options.cwd.wstring();
    const BOOL created = CreateProcessW(script ? nullptr : exe.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data(),
                                        cwd.empty() ? nullptr : cwd.c_str(), &startup, &info);
    CloseHandle(stdinRead);
    CloseHandle(stdoutWrite);
    CloseHandle(stderrWrite);
    if (!created)
    {
        error = std::format("cannot start {}: {}", options.executable.string(), LastErrorText());
        CloseHandle(stdinWrite);
        CloseHandle(stdoutRead);
        CloseHandle(stderrRead);
        return false;
    }
    m_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    AssignProcessToJobObject(m_job, info.hProcess);
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);

    m_process = info.hProcess;
    m_pid = info.dwProcessId;
    m_stdinWrite = stdinWrite;
    m_stdoutRead = stdoutRead;
    m_stderrRead = stderrRead;
    m_eof = false;
    m_exitReported = false;
    m_stdoutThread = std::thread([this] { ReadStdout(); });
    m_stderrThread = std::thread([this] { ReadStderr(); });
    return true;
}

void ClaudeProcess::ReadStdout()
{
    std::string pending;
    char buffer[65536];
    DWORD read = 0;
    while (ReadFile(m_stdoutRead, buffer, sizeof(buffer), &read, nullptr) && read > 0)
    {
        pending.append(buffer, read);
        size_t start = 0;
        for (size_t nl; (nl = pending.find('\n', start)) != std::string::npos; start = nl + 1)
        {
            std::string_view line(pending.data() + start, nl - start);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            if (line.empty())
                continue;
            Json event = ParseJson(line);
            std::lock_guard lock(m_mutex);
            if (event.is_object())
                m_events.push_back(std::move(event));
            else
            {
                // Non-JSON output (e.g. `--version`, warnings): kept with stderr for diagnostics.
                m_stderr.emplace_back(line);
                if (m_stderr.size() > 40)
                    m_stderr.pop_front();
            }
        }
        pending.erase(0, start);
    }
    m_eof = true;
}

void ClaudeProcess::ReadStderr()
{
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(m_stderrRead, buffer, sizeof(buffer), &read, nullptr) && read > 0)
    {
        std::lock_guard lock(m_mutex);
        m_stderr.emplace_back(buffer, read);
        if (m_stderr.size() > 40)
            m_stderr.pop_front();
    }
}

void ClaudeProcess::WriteLine(const Json& message)
{
    if (!m_stdinWrite)
        return;
    const std::string line = message.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
    std::lock_guard lock(m_writeMutex);
    DWORD written = 0;
    WriteFile(m_stdinWrite, line.data(), DWORD(line.size()), &written, nullptr);
}

void ClaudeProcess::Poll(std::vector<Json>& events, bool& exited, unsigned long& exitCode)
{
    {
        std::lock_guard lock(m_mutex);
        events.insert(events.end(), std::make_move_iterator(m_events.begin()), std::make_move_iterator(m_events.end()));
        m_events.clear();
    }
    exited = false;
    if (m_process && m_eof && !m_exitReported)
    {
        WaitForSingleObject(m_process, 2000);
        DWORD code = 0;
        GetExitCodeProcess(m_process, &code);
        exitCode = code;
        exited = true;
        m_exitReported = true;
    }
}

std::string ClaudeProcess::StderrTail() const
{
    std::lock_guard lock(m_mutex);
    std::string text;
    for (const std::string& s : m_stderr)
        text += s + (s.ends_with('\n') ? "" : "\n");
    return text;
}

void ClaudeProcess::Stop()
{
    if (!m_process)
        return;
    if (m_stdinWrite)
    {
        CloseHandle(m_stdinWrite);
        m_stdinWrite = nullptr;
    }
    // Closing the job kills the CLI and every child it started (subagents, MCP servers).
    if (m_job)
    {
        TerminateJobObject(m_job, 1);
        CloseHandle(m_job);
        m_job = nullptr;
    }
    // The readers end when the pipes close; a process that left the job (or inherited a pipe) may keep
    // them open, so their blocking reads are cancelled until they return.
    for (std::thread* reader : {&m_stdoutThread, &m_stderrThread})
        if (reader->joinable())
        {
            const HANDLE thread = HANDLE(reader->native_handle());
            for (int i = 0; i < 200 && WaitForSingleObject(thread, 10) == WAIT_TIMEOUT; ++i)
                CancelSynchronousIo(thread);
            reader->join();
        }
    CloseHandle(m_stdoutRead);
    CloseHandle(m_stderrRead);
    CloseHandle(m_process);
    m_stdoutRead = m_stderrRead = m_process = nullptr;
    m_pid = 0;
}

}  // namespace aiad::ai
