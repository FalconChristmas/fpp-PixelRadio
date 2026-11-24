#include <fpp-pch.h>

#include <string>
#include <string_view>
#include <vector>
#include <queue>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <unistd.h>
#include <termios.h>
#include <sys/stat.h>

#include "mediadetails.h"
#include "common.h"
#include "settings.h"
#include "Plugin.h"
#include "log.h"
#include "commands/Commands.h"


class FPPPixelRadioPlugin;


class PixelRadioCarrierCommand : public Command {
public:
    PixelRadioCarrierCommand(FPPPixelRadioPlugin *p);
    virtual ~PixelRadioCarrierCommand() {}

    virtual std::unique_ptr<Result> run(const std::vector<std::string>& args) override;
    FPPPixelRadioPlugin *plugin;
};


class FPPPixelRadioPlugin : public FPPPlugins::Plugin, public FPPPlugins::PlaylistEventPlugin {
public:
    std::string baseURL;
    // False until an IPAddress is configured.  The plugin ships with it empty
    // and every install that never sets one built "http://:8080/cmd?" and sent
    // it anyway -- a PSN every StationIDTime and an RTM every RDSCycleTime,
    // each failing instantly on a malformed URL and each writing a line to
    // fppd.log.  It never reached the network, so it cost no time, but it
    // buried real log entries and churned the SD card indefinitely.
    bool configured = false;
    bool warnedUnconfigured = false;

    
    std::string stationName = "";
    std::string rdsText = "";
    std::string rdsText2 = "";
    std::string rdsText3 = "";

    volatile bool running = true;
    // Set by shutdown(): the sender finishes what is already queued and then
    // exits, rather than picking up another cycle. 'finished' is what the
    // readiness predicate handed back to FPP reports on. Both under 'lock'.
    bool stopping = false;
    bool finished = false;
    std::mutex lock;
    std::condition_variable condition;
    std::thread *sendThread = nullptr;
    PixelRadioCarrierCommand *carrierCommand = nullptr;
    std::queue<std::string> urls;

    uint32_t stationIdCycleTime = 5;
    uint32_t rdsCycleTime = 15;

    std::vector<std::string> stationIdStrings;
    int curStationIdString = 0;
    uint64_t nextStationTime = 0;

    std::array<std::string, 3> rdsStrings;
    int curRDSString = 0;
    uint64_t nextRDSTime = 0;

    // ---- Plugin log file ----------------------------------------------------
    // fppd.log only offers one level for every plugin at once, so this plugin
    // can also keep its own log, logs/fpp-PixelRadio.log, with its own choice
    // of what goes in it. Nothing here changes what goes to fppd.log.
    //
    // Written from both the main loop (settings, playlist and media callbacks)
    // and the sender thread, so the file and the options that decide what is
    // written are all guarded by logLock - never held while taking 'lock'.
    enum LogCommandsLevel { LOG_CMDS_OFF = 0, LOG_CMDS_CONFIG = 1, LOG_CMDS_ALL = 2 };
    std::mutex logLock;
    FILE *logFile = nullptr;
    std::string logFileName;
    bool logToFile = false;
    int logCommands = LOG_CMDS_CONFIG;
    bool logRDSText = false;
    bool logConnection = true;
    bool logSettingChanges = true;
    long logMaxSize = 1024 * 1024;
    // Last text logged per slot (0 = station ID, 1-3 = RDS strings), so a new
    // song logs only the text that actually changed. Guarded by logLock.
    std::array<std::string, 4> lastLoggedText;

    // Sender thread only: whether the last request to the radio succeeded
    // (-1 = not tried yet) and which address that was, so a changed IP or
    // port starts the online/offline tracking afresh.
    int radioOnline = -1;
    std::string radioBase;

    // The "true" asks FPP to watch config/plugin.fpp-PixelRadio and call
    // settingChanged() below, so retuning the radio no longer needs an fppd
    // restart.
    FPPPixelRadioPlugin() : FPPPlugins::Plugin("fpp-PixelRadio", true), FPPPlugins::PlaylistEventPlugin() {
        setDefaultSettings();
        applyLogSettings();
        writeLog("PixelRadio plugin started (radio %s:%s, %s MHz)",
                 settings["IPAddress"].empty() ? "<not set>" : settings["IPAddress"].c_str(),
                 settings["Port"].c_str(), settings["Frequency"].c_str());
        applyConfiguration();
        sendThread = new std::thread([this] () {this->run();});
        condition.notify_all();

        // PixelRadioCarrierCommand is declared in this plugin - its vtable lives
        // in this .so and it holds a back-pointer here - so shutdown() takes it
        // back rather than leaving it registered against an unloaded plugin.
        carrierCommand = new PixelRadioCarrierCommand(this);
        CommandManager::INSTANCE.addCommand(carrierCommand);
    }

    // Push the current settings to the radio and refresh the text being cycled.
    // Safe to call again while the sender thread is running - everything shared
    // with it is written under 'lock', and the radio commands simply join the
    // queue it is already draining - which is what lets a settings change take
    // effect without restarting fppd.
    void applyConfiguration() {
        {
            std::unique_lock<std::mutex> lk(lock);
            baseURL = "http://" + settings["IPAddress"] + ":" + settings["Port"] + "/cmd?";
            // Re-evaluated on every settings change, so setting the IP starts
            // the radio talking without an fppd restart, and clearing it stops
            // the traffic again (and re-arms the warning).
            const bool wasConfigured = configured;
            configured = !settings["IPAddress"].empty();
            if (configured != wasConfigured) {
                warnedUnconfigured = false;
            }
            // Settings are user input, and this now runs whenever one changes
            // rather than only at startup, so a throw here would land on the
            // main loop inside FPP's file-monitor callback and take fppd down.
            stationIdCycleTime = safeStoi(settings["StationIDTime"], 5, "StationIDTime");
            rdsCycleTime = safeStoi(settings["RDSCycleTime"], 15, "RDSCycleTime");

            std::string freq = settings["Frequency"];
            replaceAll(freq, ".", "");
            urls.emplace("freq=" + freq);
            urls.emplace(stationCodeCommand());
            urls.emplace("start=rds");
            urls.emplace("pty=" + settings["ProgramType"]);

            // Restart the cycles from the top so the new text is what goes out
            // next, rather than continuing part way through the old set.
            curStationIdString = 0;
            curRDSString = 0;
            nextStationTime = 0;
            nextRDSTime = 0;
        }
        stopAction();
        formatAndSendText(settings["StationID"], "", "", "", 0, 0);
        formatAndSendText(settings["RDS"], "", "", "", 0, 1);
        formatAndSendText(settings["RDS2"], "", "", "", 0, 2);
        formatAndSendText(settings["RDS3"], "", "", "", 0, 3);
        condition.notify_all();
    }

    // Called by FPP when config/plugin.fpp-PixelRadio changes; the base class
    // has already updated settings[key]. Every setting this plugin has feeds
    // the radio configuration or the cycled text, and re-sending the lot is a
    // handful of queued commands, so there is nothing to be gained by treating
    // them individually. IdleAction is read at point of use in start/stopAction()
    // and so needs nothing here at all.
    virtual void settingChanged(const std::string &key, const std::string &value) override {
        if (key.rfind("Log", 0) == 0) {
            // Logging options affect only this plugin's log file; there is no
            // reason to re-send the radio its whole configuration for them.
            LogInfo(VB_PLUGIN, "PixelRadio: logging option %s changed\n", key.c_str());
            applyLogSettings();
            if (shouldLogSettingChanges()) {
                writeLog("Logging option %s set to \"%s\"", key.c_str(), value.c_str());
            }
            return;
        }
        LogInfo(VB_PLUGIN, "PixelRadio: %s changed, reconfiguring\n", key.c_str());
        if (shouldLogSettingChanges()) {
            writeLog("Setting %s changed to \"%s\"", key.c_str(), value.c_str());
        }
        applyConfiguration();
    }

    // Re-read the logging options. Main loop only (it reads 'settings').
    void applyLogSettings() {
        std::unique_lock<std::mutex> lk(logLock);
        logToFile = settings["LogToFile"] == "1";
        logCommands = (int)safeStoi(settings["LogCommands"], LOG_CMDS_CONFIG, "LogCommands");
        logRDSText = settings["LogRDSText"] == "1";
        logConnection = settings["LogConnection"] == "1";
        logSettingChanges = settings["LogSettingChanges"] == "1";
        uint32_t kb = safeStoi(settings["LogMaxSizeKB"], 1024, "LogMaxSizeKB");
        if (kb < 64) {
            kb = 64;
        }
        logMaxSize = (long)kb * 1024;
        if (!logToFile && logFile) {
            fclose(logFile);
            logFile = nullptr;
        }
    }

    bool shouldLogSettingChanges() {
        std::unique_lock<std::mutex> lk(logLock);
        return logToFile && logSettingChanges;
    }

    // Commands sent on every cycle (station ID fragments and RDS text), as
    // opposed to the occasional configuration commands.
    static bool isCycleCommand(const std::string &u) {
        return u.rfind("psn=", 0) == 0 || u.rfind("rtm=", 0) == 0 || u.rfind("rtper=", 0) == 0;
    }

    // Caller holds logLock. Opens the log on first use and rotates it to .1
    // once it reaches the size limit.
    bool openLogFileLocked() {
        if (logFile) {
            long sz = ftell(logFile);
            if (sz >= 0 && sz < logMaxSize) {
                return true;
            }
            fclose(logFile);
            logFile = nullptr;
            std::string old = logFileName + ".1";
            unlink(old.c_str());
            rename(logFileName.c_str(), old.c_str());
        }
        if (logFileName.empty()) {
            logFileName = FPP_DIR_MEDIA("/logs/fpp-PixelRadio.log");
        }
        logFile = fopen(logFileName.c_str(), "a");
        if (logFile) {
            // So ftell() above measures the existing file, not just this run.
            fseek(logFile, 0, SEEK_END);
        }
        if (!logFile) {
            LogErr(VB_PLUGIN, "PixelRadio: unable to open %s: %s - log file disabled until the option is saved again\n",
                   logFileName.c_str(), strerror(errno));
            logToFile = false;
            return false;
        }
        return true;
    }

    // Write one line to the plugin log, if it is enabled. Callers check the
    // per-category option themselves; this only checks the master switch.
    __attribute__((format(printf, 2, 3)))
    void writeLog(const char *format, ...) {
        std::unique_lock<std::mutex> lk(logLock);
        if (!logToFile || !openLogFileLocked()) {
            return;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm;
        localtime_r(&ts.tv_sec, &tm);
        char tbuf[32];
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);
        fprintf(logFile, "%s.%03ld ", tbuf, ts.tv_nsec / 1000000);

        va_list args;
        va_start(args, format);
        vfprintf(logFile, format, args);
        va_end(args);
        fputc('\n', logFile);
        fflush(logFile);
    }

    // Sender thread: log a command and its result according to the options,
    // and track whether the radio is answering.
    void logCommandResult(const std::string &base, const std::string &u, bool ok, const std::string &resp) {
        bool logCmd;
        bool logConn;
        {
            std::unique_lock<std::mutex> lk(logLock);
            logCmd = logToFile && (logCommands == LOG_CMDS_ALL ||
                                   (logCommands == LOG_CMDS_CONFIG && !isCycleCommand(u)));
            logConn = logToFile && logConnection;
        }
        if (logCmd) {
            if (ok) {
                writeLog("Sent %s -> %s", u.c_str(), resp.empty() ? "(empty response)" : resp.c_str());
            } else {
                writeLog("FAILED %s (no response from %s)", u.c_str(), base.c_str());
            }
        }

        if (base != radioBase) {
            radioBase = base;
            radioOnline = -1;
        }
        const int now = ok ? 1 : 0;
        if (now == radioOnline) {
            return;
        }
        const int was = radioOnline;
        radioOnline = now;
        if (!ok) {
            // fppd.log gets this too, whatever the file options: an offline
            // radio is otherwise only visible as FPP's generic curl errors.
            LogWarn(VB_PLUGIN, "PixelRadio: radio at %s is not responding\n", base.c_str());
            if (logConn) {
                writeLog("Radio at %s is NOT responding", base.c_str());
            }
        } else if (was == 0) {
            LogInfo(VB_PLUGIN, "PixelRadio: radio at %s is responding again\n", base.c_str());
            if (logConn) {
                writeLog("Radio at %s is responding again", base.c_str());
            }
        } else if (logConn) {
            writeLog("Radio at %s is responding", base.c_str());
        }
    }

    // Main loop: log RDS / station ID text when it differs from what was last
    // logged for that slot. 'text' is the plain (not URL-encoded) text.
    void logTextChange(int location, const std::string &text) {
        {
            std::unique_lock<std::mutex> lk(logLock);
            if (!logToFile || !logRDSText || lastLoggedText[location] == text) {
                return;
            }
            lastLoggedText[location] = text;
        }
        std::string shown = text.empty() ? "(empty)" : "\"" + text + "\"";
        if (location == 0) {
            writeLog("Station ID text: %s", shown.c_str());
        } else {
            writeLog("RDS text #%d: %s", location, shown.c_str());
        }
    }

    // "pic=0x...", the RDS programme identification code derived from the
    // station's call sign. Caller holds 'lock'.
    std::string stationCodeCommand() {
        std::string sc = settings["StationCode"];
        while (sc.length() < 4) {
            sc += "A";
        }
        int v2 = sc[1] - 65;
        int v3 = sc[2] - 65;
        int v4 = sc[3] - 65;
        if (v2 < 0 || v2 > 26) v2 = 0;
        if (v3 < 0 || v3 > 26) v3 = 0;
        if (v4 < 0 || v4 > 26) v4 = 0;
        int v1 = (sc[0] == 'W') ? 21672 : 4096;
        v1 += v4;
        v1 += v3 * 26;
        v1 += v2 * 26 * 26;
        char buf[32];
        snprintf(buf, sizeof(buf), "pic=0x%X", v1);
        return buf;
    }

    static uint32_t safeStoi(const std::string &s, uint32_t defVal, const char *name) {
        try {
            if (!s.empty()) {
                return (uint32_t)std::stoi(s);
            }
        } catch (const std::exception &e) {
            LogErr(VB_PLUGIN, "PixelRadio: bad value for %s (\"%s\"): %s - using %u\n",
                   name, s.c_str(), e.what(), defVal);
        }
        return defVal;
    }

    // Ask the sender to send its last "stop=rds", drain whatever else is queued
    // and exit - but do NOT wait for it here. Each queued entry is an HTTP
    // request to the radio, so if the radio is unreachable they sit in curl
    // timeouts; the old destructor spun on the queue waiting for exactly that,
    // which on an unload would stall fppd's main loop mid-show. FPP polls the
    // predicate instead and destroys the plugin once the thread has finished.
    virtual std::function<bool()> shutdown() override {
        if (carrierCommand) {
            // removeCommand() only unregisters; CommandManager deletes whatever
            // is still registered at shutdown, so taking it back means owning it.
            CommandManager::INSTANCE.removeCommand(carrierCommand);
            delete carrierCommand;
            carrierCommand = nullptr;
        }
        {
            std::unique_lock<std::mutex> lk(lock);
            if (!sendThread) {
                return nullptr;
            }
            urls.emplace("stop=rds");
            stopping = true;
        }
        condition.notify_all();
        return [this]() {
            std::unique_lock<std::mutex> lk(lock);
            return finished;
        };
    }

    virtual ~FPPPixelRadioPlugin() {
        // By the time FPP destroys the plugin the predicate above has reported
        // the sender finished, so this join returns at once. It stays
        // unconditional so a teardown that never went through shutdown() still
        // stops the thread rather than leaving it running against a destroyed
        // object - the thread body touches members of this class.
        {
            std::unique_lock<std::mutex> lk(lock);
            running = false;
            stopping = true;
        }
        condition.notify_all();
        if (sendThread) {
            if (sendThread->joinable()) {
                sendThread->join();
            }
            delete sendThread;
            sendThread = nullptr;
        }
        std::unique_lock<std::mutex> lk(logLock);
        if (logFile) {
            fclose(logFile);
            logFile = nullptr;
        }
    }
    void run() {
        std::unique_lock<std::mutex> lk(lock);
        while (running) {
            uint64_t ct = GetTimeMS();
            // Once shutting down, stop queueing new cycles - just flush what is
            // already there (including shutdown()'s "stop=rds") and leave.
            if (!stopping && ct > nextStationTime && curStationIdString < stationIdStrings.size()) {
                urls.emplace("psn=" + urlencode(stationIdStrings[curStationIdString]));
                curStationIdString++;
                if (curStationIdString >= stationIdStrings.size()) {
                    curStationIdString = 0;
                }
                nextStationTime = ct + stationIdCycleTime * 1000;
            }
            if (!stopping && ct > nextRDSTime) {
                std::string s = rdsStrings[curRDSString];
                if (s == "") {
                    s = "%7f";
                }
                urls.emplace("rtm=" + s);
                curRDSString++;
                if (curRDSString == 3 || rdsStrings[curRDSString] == "") {
                    curRDSString = 0;
                }
                s = "rtper=";
                s += std::to_string(rdsCycleTime);
                urls.emplace(s);
                nextRDSTime = ct + rdsCycleTime * 1000;
            }

            while (!urls.empty()) {
                std::string resp;
                std::string u = urls.front();
                urls.pop();
                // Copy while holding the lock: applyConfiguration() can change
                // baseURL from the main loop when the IP or port setting does.
                std::string base = baseURL;
                const bool send = configured;
                const bool warn = !configured && !warnedUnconfigured;
                if (warn) {
                    warnedUnconfigured = true;
                }
                lk.unlock();
                if (send) {
                    const bool ok = urlGet(base + u, resp);
                    LogDebug(VB_PLUGIN, "%s%s:   %s", base.c_str(), u.c_str(), resp.c_str());
                    logCommandResult(base, u, ok, resp);
                } else if (warn) {
                    // Once per transition, not once per URL: the point is to
                    // stop filling the log, so the fix must not fill it either.
                    LogWarn(VB_PLUGIN, "PixelRadio: no IPAddress configured; "
                            "not sending RDS commands until one is set\n");
                    writeLog("No IP Address configured - not sending commands to the radio until one is set");
                }
                lk.lock();
            }
            // Queue drained and we were asked to stop: nothing left to send.
            if (stopping) {
                break;
            }
            if (running && urls.empty()) {
                condition.wait_for(lk, std::chrono::seconds(1));
            }
        }
        running = false;
        finished = true; // still holding 'lock', which the predicate takes
        lk.unlock();
        condition.notify_all();
    }
    void startAction() {
        if (settings["IdleAction"] == "1") {
            std::unique_lock<std::mutex> lk(lock);
            urls.emplace("mute=off");
        } else if (settings["IdleAction"] == "2") {
            std::unique_lock<std::mutex> lk(lock);
            urls.emplace("rfc=on");
        }
        condition.notify_all();
    }
   
    void stopAction() {
        if (settings["IdleAction"] == "1") {
            std::unique_lock<std::mutex> lk(lock);
            urls.emplace("mute=on");
        } else if (settings["IdleAction"] == "2") {
            std::unique_lock<std::mutex> lk(lock);
            urls.emplace("rfc=off");
        }
        condition.notify_all();
    }
    void AddURL(const std::string &s) {
        std::unique_lock<std::mutex> lk(lock);
        urls.emplace(s);
        condition.notify_all();
    }
    
    static void padTo(std::string &s, int l) {
        size_t n = l - s.size();
        if (n) {
            s.append(n, ' ');
        }
    }

    std::string urlencode(const std::string &s)
    {
        static const char lookup[]= "0123456789abcdef";
        std::stringstream e;
        for(int i=0, ix=s.length(); i<ix; i++)
        {
            const char& c = s[i];
            if ( (48 <= c && c <= 57) ||//0-9
                 (65 <= c && c <= 90) ||//abc...xyz
                 (97 <= c && c <= 122) || //ABC...XYZ
                 (c=='-' || c=='_' || c=='.' || c=='~') 
            )
            {
                e << c;
            }
            else
            {
                e << '%';
                e << lookup[ (c&0xF0)>>4 ];
                e << lookup[ (c&0x0F) ];
            }
        }
        return e.str();
    }

    void formatAndSendText(const std::string &text, const std::string &artist, const std::string &title, const std::string &album, int length, int location) {
        std::string output;
        
        int artistIdx = -1;
        int titleIdx = -1;
        int albumIdx = -1;

        for (int x = 0; x < text.length(); x++) {
            if (text[x] == '[') {
                if (artist == "" && title == "") {
                    while (text[x] != ']' && x < text.length()) {
                        x++;
                    }
                }
            } else if (text[x] == ']') {
                //nothing
            } else if (text[x] == '{') {
                // constexpr string_view, not const static std::string: a
                // static local in an inline (in-class) function is emitted
                // as an STB_GNU_UNIQUE symbol, and glibc permanently refuses
                // to unload any library that first defines one - dlclose()
                // then succeeds and unmaps nothing. These need no dynamic
                // initialisation, so they produce no guard variable at all.
                constexpr std::string_view ARTIST = "{Artist}";
                constexpr std::string_view TITLE = "{Title}";
                constexpr std::string_view ALBUM = "{Album}";
                std::string subs = text.substr(x);
                if (subs.rfind(ARTIST) == 0) {
                    artistIdx = output.length();
                    x += ARTIST.length() - 1;
                    output += artist;
                } else if (subs.rfind(TITLE) == 0) {
                    titleIdx = output.length();
                    x += TITLE.length() - 1;
                    output += title;
                } else if (subs.rfind(ALBUM) == 0) {
                    titleIdx = output.length();
                    x += ALBUM.length() - 1;
                    output += album;
                } else {
                    output += text[x];
                }
            } else {
                output += text[x];
            }
        }
        logTextChange(location, output);
        output = urlencode(output);
        if (location == 0) {
            LogDebug(VB_PLUGIN, "Setting RDS Station text to \"%s\"\n", output.c_str());
            std::vector<std::string> fragments;
            while (output.size()) {
                if (output.size() <= 8) {
                    padTo(output, 8);
                    fragments.push_back(output);
                    output.clear();
                } else {
                    std::string lft = output.substr(0, 8);
                    padTo(lft, 8);
                    output = output.substr(8);
                    fragments.push_back(lft);
                }
            }
            if (fragments.empty()) {
                std::string m = "        ";
                fragments.push_back(m);
            }
            std::unique_lock<std::mutex> lk(lock);
            stationIdStrings = fragments;
            curStationIdString = 0;
            lk.unlock();
            condition.notify_all();
        } else {
            LogDebug(VB_PLUGIN, "Setting RDS %d text to \"%s\"\n", location, output.c_str());
            std::unique_lock<std::mutex> lk(lock);
            rdsStrings[location - 1] = output;
            curRDSString = 0;
            lk.unlock();
            condition.notify_all();
        }
    }
    
    virtual void playlistCallback(const Json::Value &playlist, const std::string &action, const std::string &section, int item) {
        if (action == "stop") {
            formatAndSendText(settings["StationID"], "", "", "", 0, 0);
            formatAndSendText(settings["RDS"], "", "", "", 0, 1);
            formatAndSendText(settings["RDS2"], "", "", "", 0, 2);
            formatAndSendText(settings["RDS3"], "", "", "", 0, 3);
            nextRDSTime = 0;
            nextStationTime = 0;
        }
        if (action == "start") {
            startAction();
        } else if (action == "stop") {
            stopAction();
        }
        
    }
    virtual void mediaCallback(const Json::Value &playlist, const MediaDetails &mediaDetails) {
        std::string title = mediaDetails.title;
        std::string artist = mediaDetails.artist;
        std::string album = mediaDetails.album;
        int track = mediaDetails.track;
        int length = mediaDetails.length;
        
        std::string type = playlist["currentEntry"]["type"].asString();
        if (type != "both" && type != "media") {
            title = "";
            artist = "";
            album = "";
        }
        
        formatAndSendText(settings["StationID"], artist, title, album, length, 0);
        formatAndSendText(settings["RDS"], artist, title, album, length, 1);
        formatAndSendText(settings["RDS2"], artist, title, album, length, 2);
        formatAndSendText(settings["RDS3"], artist, title, album, length, 3);
        nextRDSTime = 0;
        nextStationTime = 0;
    }
    
    
    void setDefaultSettings() {
        setIfNotFound("Frequency", "87.9");
        setIfNotFound("IdleAction", "0");
        setIfNotFound("IPAddress", "");
        setIfNotFound("Port", "8080");

        setIfNotFound("StationID", "Merry   Christ- mas", true);        
        setIfNotFound("RDS", "[{Artist} - {Title}]", true);
        setIfNotFound("RDS2", "", true);
        setIfNotFound("RDS3", "", true);
        setIfNotFound("RDSCycleTime", "15");
        setIfNotFound("StationIDTime", "5");
        setIfNotFound("ProgramType", "0");
        setIfNotFound("StationCode", "WFPP");

        setIfNotFound("LogToFile", "0");
        setIfNotFound("LogCommands", "1");
        setIfNotFound("LogRDSText", "0");
        setIfNotFound("LogConnection", "1");
        setIfNotFound("LogSettingChanges", "1");
        setIfNotFound("LogMaxSizeKB", "1024");
    }
    void setIfNotFound(const std::string &s, const std::string &v, bool emptyAllowed = false) {
        if (settings.find(s) == settings.end()) {
            settings[s] = v;
        } else if (!emptyAllowed && settings[s] == "") {
            settings[s] = v;
        }
        LogDebug(VB_PLUGIN, "Setting \"%s\": \"%s\"\n", s.c_str(), settings[s].c_str());
    }
};



PixelRadioCarrierCommand::PixelRadioCarrierCommand(FPPPixelRadioPlugin *p) 
    : Command("PixelRadio Carrier", "PixelRadio - Enable/Disable Carrier"), plugin(p)
{
        args.push_back(CommandArg("On/Off", "bool", "Enable/Disable Carrier"));
}

std::unique_ptr<Command::Result> PixelRadioCarrierCommand::run(const std::vector<std::string>& args) {
    std::string s = "1";
    if (args.size() >= 1) {
        s = args[0];
    }
    if (s == "true" || s == "1") {
        plugin->AddURL("rfc=on");
    } else {
        plugin->AddURL("rfc=off");
    }
    return std::make_unique<Command::Result>("OK");
}

// Safe to dlclose() on unload: the only thread is the URL sender, and FPP does
// not destroy the plugin (nor unmap the library) until shutdown()'s predicate
// reports it finished and the destructor has joined it. urlGet() is FPP's own
// synchronous helper, not a CurlManager request, so there is nothing in flight
// to cancel. No timers, no epoll descriptors, no HTTP routes; shutdown() also
// withdraws and deletes the carrier command registered in the constructor.
FPP_PLUGIN_SUPPORTS_UNLOAD()

extern "C" {
    FPPPlugins::Plugin *createPlugin() {
        return new FPPPixelRadioPlugin();
    }
}
