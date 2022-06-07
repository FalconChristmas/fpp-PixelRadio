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

class FPPPixelRadioPlugin : public FPPPlugin {
public:
    std::string baseURL;

    
    std::string stationName = "";
    std::string rdsText = "";

    volatile bool running = true;
    std::mutex lock;
    std::condition_variable condition;
    std::thread *sendThread = nullptr;
    std::queue<std::string> urls;
    

    FPPPixelRadioPlugin() : FPPPlugin("fpp-PixelRadio") {
        setDefaultSettings();

        baseURL = "http://" + settings["IPAddress"] + ":" + settings["Port"] + "/cmd?";

        std::string freq = settings["Frequency"];
        replaceAll(freq, ".", "");

        sendThread = new std::thread([this] () {this->run();});

        std::unique_lock<std::mutex> lk(lock);
        urls.emplace("freq=" + freq);
        
        std::string sc = settings["ProgramCode"];
        while (sc.length() < 4) {
            sc += "A";
        }
        int v2 = sc[1] - 65;
        int v3 = sc[2] - 65;
        int v4 = sc[3] - 65;
        if (v2 < 0 || v2 > 26) v2 = 0;
        if (v3 < 0 || v3 > 26) v3 = 0;
        if (v4 < 0 || v4 > 26) v4 = 0;
        int v1 = (sc[0] == 'K') ? 21672 : 4096;
        v1 += v4;
        v1 += v3 * 26;
        v1 += v2 * 26 * 26;
        char buf[32];
        sprintf(buf, "pic=0x%X", v1);
        urls.emplace(buf);
        urls.emplace("start=rds");

        lk.unlock();
        condition.notify_all();

        //urlGet(baseURL + "ptype=" + settings["ProgramType"], resp);

        formatAndSendText(settings["StationID"], "", "", "", 0, true);
        formatAndSendText(settings["RDS"], "", "", "", 0, false);
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
        if (sendThread->joinable()) {
            sendThread->join();
        }
        delete sendThread;
    }
    void run() {
        std::unique_lock<std::mutex> lk(lock);
        while (running) {
            while (!urls.empty()) {
                std::string resp;
                std::string u = urls.front();
                urls.pop();
                lk.unlock();
                urlGet(baseURL + u, resp);
                lk.lock();
            }
            if (urls.empty()) {
                condition.wait(lk);
            }
        }
    }
    void startAction() {
        if (settings["IdleAction"] == "1") {
            std::unique_lock<std::mutex> lk(lock);
            urls.emplace("mute=off");
        }
        condition.notify_all();
    }
   
    void stopAction() {
        if (settings["IdleAction"] == "1") {
            std::unique_lock<std::mutex> lk(lock);
            urls.emplace("mute=on");
        }
        condition.notify_all();
    }
    
    static void padTo(std::string &s, int l) {
        size_t n = l - s.size();
        if (n) {
            s.append(n, ' ');
        }
    }

    void formatAndSendText(const std::string &text, const std::string &artist, const std::string &title, const std::string &album, int length, bool station) {
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
        if (station) {
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
            //FIXME - PixelRadio only supports a single fragment?
            if (fragments[0] != stationName) {
                stationName = fragments[0];
                std::unique_lock<std::mutex> lk(lock);
                urls.emplace("psn=" + fragments[0]);
                lk.unlock();
                condition.notify_all();
            }
        } else {
            LogDebug(VB_PLUGIN, "Setting RDS text to \"%s\"\n", output.c_str());
            if (output != rdsText) {
                rdsText = output;
                std::unique_lock<std::mutex> lk(lock);
                if (output == "") {
                    urls.emplace("rtm=%20");
                    urls.emplace("rtper=1");
                } else {
                    urls.emplace("rtm=" + output);
                    urls.emplace("rtper=900");
                }
                lk.unlock();
                condition.notify_all();
            }
        }
    }
    
    virtual void playlistCallback(const Json::Value &playlist, const std::string &action, const std::string &section, int item) {
        if (action == "stop") {
            formatAndSendText(settings["StationID"], "", "", "", 0, true);
            formatAndSendText(settings["RDS"], "", "", "", 0, false);
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
        
        formatAndSendText(settings["StationID"], artist, title, album, length, true);
        formatAndSendText(settings["RDS"], artist, title, album, length, false);
    }
    
    
    void setDefaultSettings() {
        setIfNotFound("Frequency", "87.9");
        setIfNotFound("IdleAction", "0");
        setIfNotFound("IPAddress", "");
        setIfNotFound("Port", "80");

        setIfNotFound("StationID", "Merry   Christ- mas", true);
        setIfNotFound("RDS", "[{Artist} - {Title}]", true);
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


extern "C" {
    FPPPlugin *createPlugin() {
        return new FPPPixelRadioPlugin();
    }
}
