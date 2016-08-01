#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>
#include <boost/program_options.hpp>
#include <boost/math/constants/constants.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/tokenizer.hpp>
#include <boost/intrusive_ptr.hpp>
#include <boost/foreach.hpp>


#include <iostream>
#include <cassert>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <fstream>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>


#include "recorder.h"
#include "p25_recorder.h"
#include "analog_recorder.h"
#include "config.h"
#include "smartnet_trunking.h"
#include "p25_trunking.h"

#include "smartnet_crc.h"
#include "smartnet_deinterleave.h"
#include "talkgroups.h"
#include "source.h"
#include "system.h"
#include "call.h"
#include "smartnet_parser.h"
#include "p25_parser.h"
#include "parser.h"

#include <osmosdr/source.h>

#include <gnuradio/uhd/usrp_source.h>
#include <gnuradio/msg_queue.h>
#include <gnuradio/message.h>
#include <gnuradio/blocks/file_sink.h>
#include <gnuradio/gr_complex.h>
#include <gnuradio/top_block.h>


using namespace std;
namespace logging = boost::log;

std::vector<Source *> sources;
std::vector<System *> systems;
std::map<long,long> unit_affiliations;

std::vector<Call *> calls;
Talkgroups *talkgroups;
string config_dir;
gr::top_block_sptr tb;
gr::msg_queue::sptr msg_queue;

volatile sig_atomic_t exit_flag = 0;
SmartnetParser *smartnet_parser;
P25Parser *p25_parser;

Config config;

bool qpsk_mod = true;
string default_mode;
/*
std::string talkgroups_file;
string default_mode;
string system_type;
string system_modulation;
std::vector<double> control_channels;
int current_control_channel = 0;
bool qpsk_mod = true;
smartnet_trunking_sptr smartnet_trunking;
p25_trunking_sptr p25_trunking;*/




void exit_interupt(int sig) { // can be called asynchronously
    exit_flag = 1; // set flag
}

unsigned GCD(unsigned u, unsigned v) {
    while ( v != 0) {
        unsigned r = u % v;
        u = v;
        v = r;
    }
    return u;
}

std::vector<float> design_filter(double interpolation, double deci) {
    float beta = 5.0;
    float trans_width = 0.5 - 0.4;
    float mid_transition_band = 0.5 - trans_width/2;

    std::vector<float> result = gr::filter::firdes::low_pass(
                                                             interpolation,
                                                             1,
                                                             mid_transition_band/interpolation,
                                                             trans_width/interpolation,
                                                             gr::filter::firdes::WIN_KAISER,
                                                             beta
                                                             );

    return result;
}

/**
 * Method name: load_config()
 * Description: <#description#>
 * Parameters: <#parameters#>
 */

void load_config()
{
  string system_modulation;
    try
    {

        const std::string json_filename = "config.json";

        boost::property_tree::ptree pt;
        boost::property_tree::read_json(json_filename, pt);
        BOOST_FOREACH( boost::property_tree::ptree::value_type  &node,pt.get_child("systems") )
        {
          System *system = new System();
          BOOST_LOG_TRIVIAL(info) << "Control Channels: ";
          BOOST_FOREACH( boost::property_tree::ptree::value_type  &sub_node,node.second.get_child("control_channels") )
          {
              double control_channel = sub_node.second.get<double>("",0);
              BOOST_LOG_TRIVIAL(info) << sub_node.second.get<double>("",0) << " ";
              system->add_control_channel(control_channel);
          }
          BOOST_LOG_TRIVIAL(info);


          system->set_system_type(node.second.get<std::string>("type"));
          BOOST_LOG_TRIVIAL(info) << "System Type: " << system->get_system_type();

          system->set_talkgroups_file(node.second.get<std::string>("talkgroupsFile",""));
          BOOST_LOG_TRIVIAL(info) << "Talkgroups File: " << system->get_talkgroups_file();
          systems.push_back(system);
        }
        config.capture_dir = pt.get<std::string>("captureDir",boost::filesystem::current_path().string());
        size_t pos = config.capture_dir.find_last_of("/");
        if(pos == config.capture_dir.length()-1)
        {
            config.capture_dir.erase(config.capture_dir.length()-1);
        }
        BOOST_LOG_TRIVIAL(info) << "Capture Directory: " << config.capture_dir;
        config_dir = pt.get<std::string>("configDir",boost::filesystem::current_path().string());
        BOOST_LOG_TRIVIAL(info) << "Config Directory: " << config_dir;
        config.upload_script = pt.get<std::string>("uploadScript","");
        BOOST_LOG_TRIVIAL(info) << "Upload Script: " << config.upload_script;
        config.upload_server = pt.get<std::string>("uploadServer","");
        BOOST_LOG_TRIVIAL(info) << "Upload Server: " << config.upload_server;
        default_mode = pt.get<std::string>("defaultMode","digital");
        BOOST_LOG_TRIVIAL(info) << "Default Mode: " << default_mode;

        boost::optional<std::string> mod_exists = pt.get_optional<std::string>("modulation");
                if (mod_exists) {
                    system_modulation = pt.get<std::string>("modulation");
                    if (boost::iequals(system_modulation, "QPSK"))
                    {
                        qpsk_mod = true;
                    } else if (boost::iequals(system_modulation, "FSK4")) {
                        qpsk_mod = false;
                    } else {
                        qpsk_mod = true;
                        BOOST_LOG_TRIVIAL(error) << "\tSystem Modulation Not Specified, assuming QPSK";
                    }
                } else {
                    qpsk_mod = true;
                }
        BOOST_FOREACH( boost::property_tree::ptree::value_type  &node,pt.get_child("sources") )
        {
            double center = node.second.get<double>("center",0);
            double rate = node.second.get<double>("rate",0);
            double error = node.second.get<double>("error",0);
            double ppm = node.second.get<double>("ppm",0);
            int gain = node.second.get<int>("gain",0);
            int if_gain = node.second.get<int>("ifGain",0);
            int bb_gain = node.second.get<int>("bbGain",0);
            double squelch_db = node.second.get<double>("squelch",0);
            std::string antenna = node.second.get<string>("antenna","");
            int digital_recorders = node.second.get<int>("digitalRecorders",0);
            int debug_recorders = node.second.get<int>("debugRecorders",0);
            int analog_recorders = node.second.get<int>("analogRecorders",0);

            std::string driver = node.second.get<std::string>("driver","");
            std::string device = node.second.get<std::string>("device","");


            BOOST_LOG_TRIVIAL(info) << "Center: " << node.second.get<double>("center",0);
            BOOST_LOG_TRIVIAL(info) << "Rate: " << node.second.get<double>("rate",0);
            BOOST_LOG_TRIVIAL(info) << "Error: " << node.second.get<double>("error",0);
            BOOST_LOG_TRIVIAL(info) << "PPM Error: " << node.second.get<double>("ppm",0);
            BOOST_LOG_TRIVIAL(info) << "Gain: " << node.second.get<int>("gain",0);
            BOOST_LOG_TRIVIAL(info) << "IF Gain: " << node.second.get<int>("ifGain",0);
            BOOST_LOG_TRIVIAL(info) << "BB Gain: " << node.second.get<int>("bbGain",0);
            BOOST_LOG_TRIVIAL(info) << "Squelch: " << node.second.get<double>("squelch",0);

            BOOST_LOG_TRIVIAL(info) << "Digital Recorders: " << node.second.get<int>("digitalRecorders",0);
            BOOST_LOG_TRIVIAL(info) << "Debug Recorders: " << node.second.get<int>("debugRecorders",0);
            BOOST_LOG_TRIVIAL(info) << "Analog Recorders: " << node.second.get<int>("analogRecorders",0);
            BOOST_LOG_TRIVIAL(info) << "driver: " << node.second.get<std::string>("driver","");


            if ((ppm!=0) && (error!=0)) {
                BOOST_LOG_TRIVIAL(info) << "Both PPM and Error should not be set at the same time. Setting Error to 0.";
                error = 0;
            }

            Source *source = new Source(center,rate,error,driver,device);
            BOOST_LOG_TRIVIAL(info) << "Max HZ: " << source->get_max_hz();
            BOOST_LOG_TRIVIAL(info) << "Min HZ: " << source->get_min_hz();
            source->set_if_gain(if_gain);
            source->set_bb_gain(bb_gain);
            source->set_gain(gain);
            source->set_antenna(antenna);
            source->set_squelch_db(squelch_db);
            if (ppm!=0){
                source->set_freq_corr(ppm);
            }
            source->create_digital_recorders(tb, digital_recorders, qpsk_mod);
            source->create_analog_recorders(tb, analog_recorders);
            source->create_debug_recorders(tb, debug_recorders);
            //std::cout << "Source - output_buffer - Min: " << source->get_src_block()->min_output_buffer(0) << " Max: " << source->get_src_block()->max_output_buffer(0) << "\n";
        		//source->get_src_block()->set_max_output_buffer(4096);

            sources.push_back(source);
        }
    }
    catch (std::exception const& e)
    {
        BOOST_LOG_TRIVIAL(error) << "Failed parsing Config: " << e.what();
    }

}


int get_total_recorders() {
    int total_recorders = 0;

    for(vector<Call *>::iterator it = calls.begin(); it != calls.end();it++) {
        Call *call = *it;

            if (call->get_recording() == true) {
                total_recorders++;
            }
    }
    return total_recorders;
}

/**
 * Method name: start_recorder
 * Description: <#description#>
 * Parameters: TrunkMessage message
 */

bool replace(std::string& str, const std::string& from, const std::string& to) {
    size_t start_pos = str.find(from);
    if(start_pos == std::string::npos)
        return false;
    str.replace(start_pos, from.length(), to);
    return true;
}

int start_recorder(Call *call) {
    Talkgroup * talkgroup = talkgroups->find_talkgroup(call->get_talkgroup());
    bool source_found = false;
    bool recorder_found = false;
    Recorder *recorder;
    Recorder *debug_recorder;

        //BOOST_LOG_TRIVIAL(error) << "\tCall created for: " << call->get_talkgroup() << "\tTDMA: " << call->get_tdma() <<  "\tEncrypted: " << call->get_encrypted() << "\tFreq: " << call->get_freq();

    if (call->get_encrypted() == false) {


        for(vector<Source *>::iterator it = sources.begin(); it != sources.end(); it++) {
            Source * source = *it;

            if ((source->get_min_hz() <= call->get_freq()) && (source->get_max_hz() >= call->get_freq())) {
                source_found = true;
                //std::cout << "Source - output_buffer - Min: " << source->get_src_block()->min_output_buffer(0) << " Max: " << source->get_src_block()->max_output_buffer(0) << "\n";

                 if (call->get_tdma()) {
                    BOOST_LOG_TRIVIAL(error) << "\tTrying to record TDMA: " << call->get_freq() << " For TG: " << call->get_talkgroup();
                    return 0;
                 }

                if (talkgroup)
                {
                    if (talkgroup->mode == 'A') {
                        recorder = source->get_analog_recorder(talkgroup->get_priority());
                    } else {
                        recorder = source->get_digital_recorder(talkgroup->get_priority());
                    }
                } else {
                    BOOST_LOG_TRIVIAL(error) << "\tTalkgroup not found: " << call->get_freq() << " For TG: " << call->get_talkgroup();
                    // A talkgroup was not found from the talkgroup file.
                     if (default_mode == "analog") {
                         recorder = source->get_analog_recorder(2) ;
                     } else {
                         recorder = source->get_digital_recorder(2);
                     }
                }

                int total_recorders = get_total_recorders();
                if (recorder) {
                    BOOST_LOG_TRIVIAL(error) << "Activating rec on src: " << source->get_device();
                    recorder->activate(call, total_recorders);
                    call->set_recorder(recorder);
                    call->set_recording(true);
                    recorder_found = true;
                } else {
                    BOOST_LOG_TRIVIAL(error) << "\tNot recording call";
                    return 0;
                }

                debug_recorder = source->get_debug_recorder();
                if (debug_recorder) {
                    debug_recorder->activate(call, total_recorders );
                    call->set_debug_recorder(debug_recorder);
                    call->set_debug_recording(true);
                    recorder_found = true;
                } else {
                    //BOOST_LOG_TRIVIAL(info) << "\tNot debug recording call";
                }
                
                if (recorder_found) {
                  // recording successfully started.
                  return 1;
                }

            }

        }
        if (!source_found) {
            BOOST_LOG_TRIVIAL(error) << "\tRecording not started because there was no source covering: " << call->get_freq() << " For TG: " << call->get_talkgroup();
            return 0;
        }
    } else {
        return 0;
        // anything for encrypted calls could go here...
    }
    return 1;
}



void stop_inactive_recorders() {


    for(vector<Call *>::iterator it = calls.begin(); it != calls.end();) {
        Call *call = *it;
        if ( call->since_last_update()  > 8.0) {
            call->end_call();
            delete call;
            it = calls.erase(it);
        } else {
            ++it;
        }//if rx is active
    }//foreach loggers
}



int retune_recorder(TrunkMessage message, Call *call) {

    Recorder *recorder = call->get_recorder();
    Source *source = recorder->get_source();

    BOOST_LOG_TRIVIAL(info) << "\tRetune - Elapsed: " << call->elapsed() << "s \tSince update: " << call->since_last_update() << "s \tTalkgroup: " << message.talkgroup << "\tOld Freq: " << call->get_freq() << "\tNew Freq: " << message.freq << "\tWav Pos: " << recorder->get_current_length();

    // set the call to the new Freq / TDMA slot
    call->set_freq(message.freq);
    call->set_tdma(message.tdma);


    if ((source->get_min_hz() <= message.freq) && (source->get_max_hz() >= message.freq)) {
        recorder->tune_offset(message.freq);
        if (call->get_debug_recording() == true) {
            call->get_debug_recorder()->tune_offset(message.freq);
        }
        return 1;
    } else {
         BOOST_LOG_TRIVIAL(info) << "\t\tSwitching to Different Source to Record";

        recorder->deactivate();

        if (call->get_debug_recording() == true) {
            call->get_debug_recorder()->deactivate();
            call->set_debug_recording(false);
        }

        int restarted_recording = start_recorder(call);
        return restarted_recording;
    }

}

void assign_recorder(TrunkMessage message) {
    bool call_found = false;
    char shell_command[200];

    // go through all the talkgroups
    for(vector<Call *>::iterator it = calls.begin(); it != calls.end();) {
        Call *call= *it;

        // Does the call have the same talkgroup
        if (call->get_talkgroup() == message.talkgroup) {
            call_found = true;
            call->update(message);

            // Is the freq the same?
            if (call->get_freq() != message.freq) {
                BOOST_LOG_TRIVIAL(trace) << "\tAssign Retune - Total calls: " << calls.size() << "\tTalkgroup: " << message.talkgroup << "\tOld Freq: " << call->get_freq() << "\tNew Freq: " << message.freq;
                // are we currently recording the call?
                if (call->get_recording() == true) {

                    int retuned = retune_recorder(message, call);
                    if (!retuned) {
                      BOOST_LOG_TRIVIAL(info) << "\tUnable to Retune";
                      call->end_call();
                      delete call;
                      it = calls.erase(it);
                    } else {
                      ++it; // move on to the next one
                    }

                // This call is not being recorded, just update the Freq and move on
                } else {
                    call->set_freq(message.freq);
                    call->set_tdma(message.tdma);
                    ++it; // move on to the next one
                }

            // The freq hasn't changed
            } else {
                ++it; // move on to the next one
            }

        // The talkgroup for the call does not match
        } else {

            // check is the freq is the same as the one being used by the call
            if ((call->get_freq() == message.freq) && (call->get_tdma() == message.tdma)) {
              //BOOST_LOG_TRIVIAL(info) << "\tFreq in use -  TG: " << message.talkgroup << "\tFreq: " << message.freq << "\tTDMA: " << message.tdma << "\t Ending Existing call\tTG: " << call->get_talkgroup() << "\tTMDA: " << call->get_tdma() << "\tElapsed: " << call->elapsed() << "s \tSince update: " << call->since_last_update();

                // if you are recording the call, stop
                if (call->get_recording() == true) {
                    BOOST_LOG_TRIVIAL(info) << "\tFreq in use -  TG: " << message.talkgroup << "\tFreq: " << message.freq << "\tTDMA: " << message.tdma << "\t Ending Existing call\tTG: " << call->get_talkgroup() << "\tTMDA: " << call->get_tdma() << "\tElapsed: " << call->elapsed() << "s \tSince update: " << call->since_last_update();
                //different talkgroups on the same freq, that is trouble
                }
                call->end_call();
                delete call;
                it = calls.erase(it);
            } else {
                ++it; // move on to the next one
            }
        }
    }


    if (!call_found) {
        Call * call = new Call(message, config);
        start_recorder(call);
        calls.push_back(call);
    }
}

/*
void add_control_channel(double control_channel) {
    if (std::find(control_channels.begin(), control_channels.end(), control_channel) != control_channels.end()) {
        control_channels.push_back(control_channel);
    }
}*/

void current_system_id(int sysid) {
    static int active_sysid = 0;
    if(active_sysid != sysid) {
        BOOST_LOG_TRIVIAL(info) << "Decoding System ID " << std::hex << std::uppercase << sysid << std::nouppercase << std::dec;
        active_sysid = sysid;
    }
}

void unit_registration(long unit) {

}

void unit_deregistration(long unit) {
    std::map<long, long>::iterator it;

    it = unit_affiliations.find (unit);
    if (it != unit_affiliations.end()) {
        unit_affiliations.erase(it);
    }
}

void group_affiliation(long unit, long talkgroup) {
    unit_affiliations[unit] = talkgroup;
}

void update_recorder(TrunkMessage message) {

    for(vector<Call *>::iterator it = calls.begin(); it != calls.end();) {
        Call *call= *it;

        if (call->get_talkgroup() == message.talkgroup) {
            // update the call, so it stays alive
            call->update(message);

            if (call->get_freq() != message.freq) {
                if (call->get_recording() == true) {
                  BOOST_LOG_TRIVIAL(trace) << "\t Update Retune - Total calls: " << calls.size() << "\tTalkgroup: " << message.talkgroup << "\tOld Freq: " << call->get_freq() << "\tNew Freq: " << message.freq;

                    // see if we can retune the recorder, sometimes you can't if there are more than one
                    int retuned = retune_recorder(message, call);
                    if (!retuned) {
                      BOOST_LOG_TRIVIAL(info) << "\tUnable to Retune";
                      call->end_call();
                      delete call;
                      it = calls.erase(it);
                      } else {
                          ++it; // move on to the next one
                    }


                } else {
                  // the Call is not recording, update and continue
                    call->set_freq(message.freq);
                    call->set_tdma(message.tdma);
                    ++it; // move on to the next one
                }
            } else {
              // the freq is the same, just update it
              ++it; // move on to the next one
            }
        } else {
          // the talkgroups don't match
          ++it; // move on to the next one
        }
    }
}

void unit_check() {
    std::map<long, long> talkgroup_totals;
    std::map<long, long>::iterator it;
    char shell_command[200];
    time_t starttime = time(NULL);
    tm *ltm = localtime(&starttime);
    char unit_filename[160];

    std::stringstream path_stream;
    path_stream << boost::filesystem::current_path().string() <<  "/" << 1900 + ltm->tm_year << "/" << 1 + ltm->tm_mon << "/" << ltm->tm_mday;

    boost::filesystem::create_directories(path_stream.str());



    for(it = unit_affiliations.begin(); it != unit_affiliations.end(); ++it) {
        talkgroup_totals[it->second]++;
    }

    sprintf(unit_filename, "%s/%ld-unit_check.json", path_stream.str().c_str(),starttime);

    ofstream myfile (unit_filename);
    if (myfile.is_open())
    {
        myfile << "{\n";
        myfile << "\"talkgroups\": {\n";
        for(it = talkgroup_totals.begin(); it != talkgroup_totals.end(); ++it) {
            if (it != talkgroup_totals.begin()) {
                myfile << ",\n";
            }
            myfile << "\"" << it->first << "\": " << it->second;

        }
        myfile << "\n}\n}\n";
        sprintf(shell_command,"./unit_check.sh %s > /dev/null 2>&1 &", unit_filename);
        int rc = system(shell_command);
        myfile.close();
    }
}

void handle_message(std::vector<TrunkMessage>  messages) {
    for(std::vector<TrunkMessage>::iterator it = messages.begin(); it != messages.end(); it++) {
        TrunkMessage message = *it;

        switch(message.message_type) {
            case GRANT:
                assign_recorder(message);
                break;
            case UPDATE:
                update_recorder(message);
                break;
            case CONTROL_CHANNEL:
                //add_control_channel(message.freq);
                break;
            case REGISTRATION:
                break;
            case DEREGISTRATION:
                unit_deregistration(message.source);
                break;
            case AFFILIATION:
                group_affiliation(message.source, message.talkgroup);
                break;
            case SYSID:
		current_system_id(message.sysid);
                break;
            case STATUS:
            case UNKNOWN:
                break;
        }
    }
}

void monitor_messages() {
    gr::message::sptr msg;
    int messagesDecodedSinceLastReport = 0;
    float msgs_decoded_per_second = 0;

    time_t lastMsgCountTime = time(NULL);;
    time_t lastTalkgroupPurge = time(NULL);;
    time_t currentTime = time(NULL);
    std::vector<TrunkMessage> trunk_messages;

    while (1) {
        if(exit_flag) { // my action when signal set it 1
            printf("\n Signal caught!\n");
            return;
        }


        msg = msg_queue->delete_head();
        messagesDecodedSinceLastReport++;
        currentTime = time(NULL);


        if ((currentTime - lastTalkgroupPurge) >= 1.0 )
        {
            stop_inactive_recorders();
            lastTalkgroupPurge = currentTime;
        }
        if (msg->to_string().find("SMARTNET: ") != std::string::npos) {
            trunk_messages = smartnet_parser->parse_message(msg->to_string().erase(0, 10));
        } else {
            trunk_messages = p25_parser->parse_message(msg);
        }

        handle_message(trunk_messages);

        float timeDiff = currentTime - lastMsgCountTime;
        if (timeDiff >= 3.0) {
            msgs_decoded_per_second = messagesDecodedSinceLastReport/timeDiff;
            messagesDecodedSinceLastReport = 0;
            lastMsgCountTime = currentTime;
            if (msgs_decoded_per_second < 10 ) {
                BOOST_LOG_TRIVIAL(error) << "\tControl Channel Message Decode Rate: " << msgs_decoded_per_second << "/sec";
            }
        }

/*
        if ((currentTime - lastUnitCheckTime) >= 300.0) {
            unit_check();
            lastUnitCheckTime = currentTime;
        }
*/
        msg.reset();



    }
}


bool monitor_system() {

  bool system_added = false;
    Source * source = NULL;
    for(vector<System *>::iterator it = systems.begin(); it != systems.end(); it++) {
      System *system = *it;
          bool source_found = false;
          double control_channel_freq = system->get_current_control_channel();
          BOOST_LOG_TRIVIAL(info) << "Control Channel: " << control_channel_freq;
          for(vector<Source *>::iterator it = sources.begin(); it != sources.end(); it++) {
              source = *it;

              if ((source->get_min_hz() <= control_channel_freq) && (source->get_max_hz() >= control_channel_freq)) {

                  // The source can cover the System's control channel, break out of the For Loop
                  source_found = true;
                  break;
              }
          }


          if (source_found) {
            system_added=true;
              if (system->get_system_type() == "smartnet") {
                  // what you really need to do is go through all of the sources to find the one with the right frequencies
                  system->smartnet_trunking = make_smartnet_trunking(control_channel_freq, source->get_center(), source->get_rate(),  msg_queue);
                  tb->connect(source->get_src_block(),0, system->smartnet_trunking, 0);
              }

              if (system->get_system_type() == "p25") {
                  // what you really need to do is go through all of the sources to find the one with the right frequencies
                  system->p25_trunking = make_p25_trunking(control_channel_freq, source->get_center(), source->get_rate(),  msg_queue, qpsk_mod);
                  tb->connect(source->get_src_block(),0, system->p25_trunking, 0);
              }
          }
    }
    return system_added;
}

int main(void)
{
    BOOST_STATIC_ASSERT(true) __attribute__((unused));
    signal(SIGINT, exit_interupt);
    logging::core::get()->set_filter
    (
     logging::trivial::severity >= logging::trivial::info
     );
    /* boost::log::add_console_log(
     cout,
     boost::log::keywords::format = "[%TimeStamp%]: %Message%",
     boost::log::keywords::auto_flush = true
   );*/
    tb = gr::make_top_block("Trunking");
    msg_queue = gr::msg_queue::make(100);
    smartnet_parser = new SmartnetParser(); // this has to eventually be generic;
    p25_parser = new P25Parser();

    load_config();

    // Setup the talkgroups from the CSV file
    talkgroups = new Talkgroups();
    //if (talkgroups_file.length() > 0) {
    BOOST_LOG_TRIVIAL(info) << "Loading Talkgroups..."<<std::endl;
    for(vector<System *>::iterator it = systems.begin(); it != systems.end(); it++) {
      System *system = *it;
      talkgroups->load_talkgroups(system->get_talkgroups_file());
    }
    //}

    if (monitor_system()) {
        tb->start();
        monitor_messages();
        //------------------------------------------------------------------
        //-- stop flow graph execution
        //------------------------------------------------------------------
        BOOST_LOG_TRIVIAL(info) << "stopping flow graph";
        tb->stop();
        tb->wait();
    } else {
        BOOST_LOG_TRIVIAL(info) << "Unable to setup Control Channel Monitor"<< std::endl;
    }

    return 0;

}
