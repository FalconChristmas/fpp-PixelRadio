#include <fpp-pch.h>

#include <string>
#include <vector>
#include <queue>

#include <unistd.h>
#include <termios.h>

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


class FPPPixelRadioPlugin : public FPPPlugin {
public:
    std::string baseURL;

    
    std::string stationName = "";
    std::string rdsText = "";
    std::string rdsText2 = "";
    std::string rdsText3 = "";

    volatile bool running = true;
    std::mutex lock;
    std::condition_variable condition;
    std::thread *sendThread = nullptr;
    std::queue<std::string> urls;

    uint32_t stationIdCycleTime = 5;
    uint32_t rdsCycleTime = 15;

    std::vector<std::string> stationIdStrings;
    int curStationIdString = 0;
    uint64_t nextStationTime = 0;

    std::array<std::string, 3> rdsStrings;
    int curRDSString = 0;
    uint64_t nextRDSTime = 0;

    FPPPixelRadioPlugin() : FPPPlugin("fpp-PixelRadio") {
        setDefaultSettings();

        stationIdCycleTime = std::stoi(settings["StationIDTime"]);
        rdsCycleTime = std::stoi(settings["RDSCycleTime"]);

        baseURL = "http://" + settings["IPAddress"] + ":" + settings["Port"] + "/cmd?";

        std::string freq = settings["Frequency"];
        replaceAll(freq, ".", "");

        std::unique_lock<std::mutex> lk(lock);
        urls.emplace("freq=" + freq);
        
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
        sprintf(buf, "pic=0x%X", v1);
        urls.emplace(buf);
        urls.emplace("start=rds");
        urls.emplace("pty=" + settings["ProgramType"]);

        lk.unlock();
        stopAction();

        formatAndSendText(settings["StationID"], "", "", "", 0, 0);
        formatAndSendText(settings["RDS"], "", "", "", 0, 1);
        formatAndSendText(settings["RDS2"], "", "", "", 0, 2);
        formatAndSendText(settings["RDS3"], "", "", "", 0, 3);
        sendThread = new std::thread([this] () {this->run();});
        condition.notify_all();

        CommandManager::INSTANCE.addCommand(new PixelRadioCarrierCommand(this));
    }

    virtual ~FPPPixelRadioPlugin() {
        urls.emplace("stop=rds");
        condition.notify_all();
        std::unique_lock<std::mutex> lk(lock);
        while (!urls.empty()) {
            lk.unlock();
            condition.notify_all();
            lk.lock();
        }
        running = false;
        lk.unlock();
        condition.notify_all();
        if (sendThread->joinable()) {
            condition.notify_all();
            sendThread->join();
        }
        delete sendThread;
    }
    void run() {
        std::unique_lock<std::mutex> lk(lock);
        while (running) {
            uint64_t ct = GetTimeMS();
            if (ct > nextStationTime && curStationIdString < stationIdStrings.size()) {
                urls.emplace("psn=" + stationIdStrings[curStationIdString]);
                curStationIdString++;
                if (curStationIdString >= stationIdStrings.size()) {
                    curStationIdString = 0;
                }
                nextStationTime = ct + stationIdCycleTime * 1000;
            }
            if (ct > nextRDSTime) {
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
                lk.unlock();
                urlGet(baseURL + u, resp);
                LogDebug(VB_PLUGIN, "%s%s:   %s", baseURL.c_str(), u.c_str(), resp.c_str());
                lk.lock();
            }
            if (running && urls.empty()) {
                condition.wait_for(lk, std::chrono::seconds(1));
            }
        }
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
                const static std::string ARTIST = "{Artist}";
                const static std::string TITLE = "{Title}";
                const static std::string ALBUM = "{Album}";
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

extern "C" {
    FPPPlugin *createPlugin() {
        return new FPPPixelRadioPlugin();
    }
}
