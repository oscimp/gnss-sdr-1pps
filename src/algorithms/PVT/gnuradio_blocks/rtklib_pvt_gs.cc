/*!
 * \file rtklib_pvt_gs.cc
 * \brief Interface of a Position Velocity and Time computation block
 * \author Javier Arribas, 2017. jarribas(at)cttc.es
 *
 * -----------------------------------------------------------------------------
 *
 * Copyright (C) 2010-2020  (see AUTHORS file for a list of contributors)
 *
 * GNSS-SDR is a software defined Global Navigation
 *          Satellite Systems receiver
 *
 * This file is part of GNSS-SDR.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * -----------------------------------------------------------------------------
 */

#include "rtklib_pvt_gs.h"
#include "MATH_CONSTANTS.h"
#include "beidou_dnav_almanac.h"
#include "beidou_dnav_ephemeris.h"
#include "beidou_dnav_iono.h"
#include "beidou_dnav_utc_model.h"
#include "display.h"
#include "galileo_almanac.h"
#include "galileo_almanac_helper.h"
#include "galileo_ephemeris.h"
#include "galileo_iono.h"
#include "galileo_utc_model.h"
#include "geojson_printer.h"
#include "glonass_gnav_almanac.h"
#include "glonass_gnav_ephemeris.h"
#include "glonass_gnav_utc_model.h"
#include "gnss_frequencies.h"
#include "gnss_sdr_create_directory.h"
#include "gnss_sdr_make_unique.h"
#include "gps_almanac.h"
#include "gps_cnav_ephemeris.h"
#include "gps_cnav_iono.h"
#include "gps_cnav_utc_model.h"
#include "gps_ephemeris.h"
#include "gps_iono.h"
#include "gps_utc_model.h"
#include "gpx_printer.h"
#include "kml_printer.h"
#include "monitor_pvt.h"
#include "monitor_pvt_udp_sink.h"
#include "nmea_printer.h"
#include "pvt_conf.h"
#include "rinex_printer.h"
#include "rtcm_printer.h"
#include "rtklib_solver.h"
#include <boost/any.hpp>                   // for any_cast, any
#include <boost/archive/xml_iarchive.hpp>  // for xml_iarchive
#include <boost/archive/xml_oarchive.hpp>  // for xml_oarchive
#include <boost/exception/diagnostic_information.hpp>
#include <boost/exception/exception.hpp>
#include <boost/serialization/map.hpp>
#include <boost/serialization/nvp.hpp>  // for nvp, make_nvp
#include <glog/logging.h>               // for LOG
#include <gnuradio/io_signature.h>      // for io_signature
#include <pmt/pmt_sugar.h>              // for mp
#include <algorithm>                    // for sort, unique
#include <cerrno>                       // for errno
#include <cstring>                      // for strerror
#include <exception>                    // for exception
#include <fstream>                      // for ofstream
#include <iomanip>                      // for put_time, setprecision
#include <iostream>                     // for operator<<
#include <locale>                       // for locale
#include <sstream>                      // for ostringstream
#include <stdexcept>                    // for length_error
#include <sys/ipc.h>                    // for IPC_CREAT
#include <sys/msg.h>                    // for msgctl
#include <typeinfo>                     // for std::type_info, typeid
#include <utility>                      // for pair


#if HAS_GENERIC_LAMBDA
#else
#include <boost/bind/bind.hpp>
#endif

#if HAS_STD_FILESYSTEM
#include <system_error>
namespace errorlib = std;
#if HAS_STD_FILESYSTEM_EXPERIMENTAL
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#else
#include <filesystem>
namespace fs = std::filesystem;
#endif
#else
#include <boost/filesystem/path.hpp>
#include <boost/system/error_code.hpp>  // for error_code
namespace fs = boost::filesystem;
namespace errorlib = boost::system;
#endif

#if USE_OLD_BOOST_MATH_COMMON_FACTOR
#include <boost/math/common_factor_rt.hpp>
namespace bc = boost::math;
#else
#include <boost/integer/common_factor_rt.hpp>
namespace bc = boost::integer;
#endif

//------------ To access B210 

#include "uhd/convert.hpp"
#include "uhd/usrp/multi_usrp.hpp"
#include "uhd/utils/safe_main.hpp"
#include <stdint.h>
#include <stdlib.h>
#include <iostream>

static const std::string GPIO_DEFAULT_GPIO       = "FP0";
uhd::usrp::multi_usrp::sptr uhd_source_;

//-------------R&S SMA 
#include "vxi11_user.h"
#pragma message "With VXI11"
#define BUF_LEN 1000
//        VXI11_CLINK *d_clink;
//-------------R&S SMA end



rtklib_pvt_gs_sptr rtklib_make_pvt_gs(uint32_t nchannels,
    const Pvt_Conf& conf_,
    const rtk_t& rtk, 
    const double PPS_Kp,
    const double PPS_Ki,
    const bool SMA_internal_source_clock,
    double LO_external_frequ,
    const bool PPS_correction,
    const bool PPS_estimator_selected,
    const std::string SMA_IP_address)
{
    return rtklib_pvt_gs_sptr(new rtklib_pvt_gs(nchannels,
        conf_,
        rtk,
	PPS_Kp,
	PPS_Ki,
	SMA_internal_source_clock,
	LO_external_frequ,
	PPS_correction,
	PPS_estimator_selected,
	SMA_IP_address));
}


rtklib_pvt_gs::rtklib_pvt_gs(uint32_t nchannels,
    const Pvt_Conf& conf_,
    const rtk_t& rtk,
    const double PPS_Kp,
    const double PPS_Ki,
    const bool SMA_internal_source_clock,
    double LO_external_frequ,
    const bool PPS_correction,
    const bool PPS_estimator_selected,
    const std::string SMA_IP_address) : gr::sync_block("rtklib_pvt_gs",
                            gr::io_signature::make(nchannels, nchannels, sizeof(Gnss_Synchro)),
                            gr::io_signature::make(0, 0, 0))
{
    char* device_name = NULL;
    char buf[BUF_LEN];
    long bytes_returned;
    char  cmd[256];
    d_PPS_correction=PPS_correction;
    if (d_PPS_correction)
	{
	    d_PPS_estimator_selected=PPS_estimator_selected;
	    d_PPS_Kp=PPS_Kp;
	    d_PPS_Ki=PPS_Ki;
	    d_LO_external_frequ=LO_external_frequ;
	    d_SMA_IP_address=SMA_IP_address;

	    std::cout 
		<< std::setprecision(9)
		<< "Kp: " << d_PPS_Kp
		<< " Ki: " << d_PPS_Ki
		<< " LO: " << d_LO_external_frequ << "[Hz]" << '\n';

	    LOG(INFO) << "vxi11 init\n";
	    std::cout
		<< "SMA IP address: " 
		<<  d_SMA_IP_address << '\n';

	    vxi11_open_device(&d_clink, d_SMA_IP_address.c_str(), device_name);
	    sprintf (cmd, "*IDN?\n");
	    vxi11_send (d_clink, cmd,strlen(cmd));
	    bytes_returned=vxi11_receive (d_clink, buf, BUF_LEN);
	    LOG(INFO) <<  buf ;
	    if (SMA_internal_source_clock)
		{
		    sprintf (cmd, "ROSC:SOUR INT");
		    vxi11_send (d_clink, cmd,strlen(cmd));
		    std::cout << "SMA Internal source clock selected\n";
		}
	    else
		{
		    sprintf (cmd, "ROSC:SOUR EXT");
		    vxi11_send (d_clink, cmd,strlen(cmd));
		    sprintf (cmd, "ROSC:EXT:FREQ 10MHz");
		    vxi11_send (d_clink, cmd,strlen(cmd));
		    std::cout << "SMA External source clock selected\n";
		}

	    sprintf(cmd, "FREQ %0.3fHz",d_LO_external_frequ);
	    vxi11_send (d_clink, cmd,strlen(cmd));



//uhd not used yet


    // variables to be set by po
	    std::string args;
	    uint32_t rb;
	    std::string gpio=GPIO_DEFAULT_GPIO; // FP0
	    uint32_t ddr;
	    uint32_t ctrl;
	    uint32_t mask;
    // create a uhd_source_ device
	    std::cout << std::endl;
	    std::cout << boost::format("Creating the uhd_source_ device with: %s...") % args
        	      << std::endl;
	    uhd_source_ = uhd::usrp::multi_usrp::make(args);

//uhd_source_
	    std::cout << boost::format("Using Device: %s") % uhd_source_->get_pp_string() << std::endl;
	    std::cout << "Using GPIO bank: " << gpio << std::endl;

	    ddr=0x00;
	    ctrl=0x00;
	    mask=0xff;
    // writing config
	    uhd_source_->set_gpio_attr(gpio, "DDR", ddr, mask);   // set data direction register (DDR)
	    uhd_source_->set_gpio_attr(gpio, "CTRL", ctrl, mask); // set control register
	    uhd_source_->set_gpio_attr(gpio, "OUT", 0x00, mask);  // set control register
	    uhd_source_->set_gpio_attr(gpio, "ATR_0X", 0x00, mask); // set ATR registers
    // reading back
	    rb= uhd_source_->get_gpio_attr(gpio, "DDR") & mask;
	    std::cout << "DDR:" << rb << std::endl;
	    rb= uhd_source_->get_gpio_attr(gpio, "CTRL") & mask;
	    std::cout << "CTRL:" << rb << std::endl;
	}

    // Send feedback message to observables block with the receiver clock offset
    this->message_port_register_out(pmt::mp("pvt_to_observables"));
    // Send PVT status to gnss_flowgraph
    this->message_port_register_out(pmt::mp("status"));

    d_mapStringValues["1C"] = evGPS_1C;
    d_mapStringValues["2S"] = evGPS_2S;
    d_mapStringValues["L5"] = evGPS_L5;
    d_mapStringValues["1B"] = evGAL_1B;
    d_mapStringValues["5X"] = evGAL_5X;
    d_mapStringValues["1G"] = evGLO_1G;
    d_mapStringValues["2G"] = evGLO_2G;
    d_mapStringValues["B1"] = evBDS_B1;
    d_mapStringValues["B2"] = evBDS_B2;
    d_mapStringValues["B3"] = evBDS_B3;

    d_initial_carrier_phase_offset_estimation_rads = std::vector<double>(nchannels, 0.0);
    d_channel_initialized = std::vector<bool>(nchannels, false);

    d_max_obs_block_rx_clock_offset_ms = conf_.max_obs_block_rx_clock_offset_ms;

    d_output_rate_ms = conf_.output_rate_ms;
    d_display_rate_ms = conf_.display_rate_ms;
    d_report_rate_ms = 1000;  // report every second PVT to gnss_synchro
    d_dump = conf_.dump;
    d_dump_mat = conf_.dump_mat and d_dump;
    d_dump_filename = conf_.dump_filename;
    std::string dump_ls_pvt_filename = conf_.dump_filename;
    if (d_dump)
        {
            std::string dump_path;
            // Get path
            if (d_dump_filename.find_last_of('/') != std::string::npos)
                {
                    std::string dump_filename_ = d_dump_filename.substr(d_dump_filename.find_last_of('/') + 1);
                    dump_path = d_dump_filename.substr(0, d_dump_filename.find_last_of('/'));
                    d_dump_filename = dump_filename_;
                }
            else
                {
                    dump_path = std::string(".");
                }
            if (d_dump_filename.empty())
                {
                    d_dump_filename = "pvt";
                }
            // remove extension if any
            if (d_dump_filename.substr(1).find_last_of('.') != std::string::npos)
                {
                    d_dump_filename = d_dump_filename.substr(0, d_dump_filename.find_last_of('.'));
                }
            dump_ls_pvt_filename = dump_path + fs::path::preferred_separator + d_dump_filename;
            dump_ls_pvt_filename.append(".dat");
            // create directory
            if (!gnss_sdr_create_directory(dump_path))
                {
                    std::cerr << "GNSS-SDR cannot create dump file for the PVT block. Wrong permissions?\n";
                    d_dump = false;
                }
        }

    d_nchannels = nchannels;

    d_type_of_rx = conf_.type_of_receiver;

    // GPS Ephemeris data message port in
    this->message_port_register_in(pmt::mp("telemetry"));
    this->set_msg_handler(pmt::mp("telemetry"),
#if HAS_GENERIC_LAMBDA
        [this](auto&& PH1) { msg_handler_telemetry(PH1); });
#else
#if USE_BOOST_BIND_PLACEHOLDERS
        boost::bind(&rtklib_pvt_gs::msg_handler_telemetry, this, boost::placeholders::_1));
#else
        boost::bind(&rtklib_pvt_gs::msg_handler_telemetry, this, _1));
#endif
#endif
    // initialize kml_printer
    const std::string kml_dump_filename = d_dump_filename;
    d_kml_output_enabled = conf_.kml_output_enabled;
    d_kml_rate_ms = conf_.kml_rate_ms;
    if (d_kml_rate_ms == 0)
        {
            d_kml_output_enabled = false;
        }
    if (d_kml_output_enabled)
        {
            d_kml_dump = std::make_unique<Kml_Printer>(conf_.kml_output_path);
            d_kml_dump->set_headers(kml_dump_filename);
        }
    else
        {
            d_kml_dump = nullptr;
        }

    // initialize gpx_printer
    const std::string gpx_dump_filename = d_dump_filename;
    d_gpx_output_enabled = conf_.gpx_output_enabled;
    d_gpx_rate_ms = conf_.gpx_rate_ms;
    if (d_gpx_rate_ms == 0)
        {
            d_gpx_output_enabled = false;
        }
    if (d_gpx_output_enabled)
        {
            d_gpx_dump = std::make_unique<Gpx_Printer>(conf_.gpx_output_path);
            d_gpx_dump->set_headers(gpx_dump_filename);
        }
    else
        {
            d_gpx_dump = nullptr;
        }

    // initialize geojson_printer
    const std::string geojson_dump_filename = d_dump_filename;
    d_geojson_output_enabled = conf_.geojson_output_enabled;
    d_geojson_rate_ms = conf_.geojson_rate_ms;
    if (d_geojson_rate_ms == 0)
        {
            d_geojson_output_enabled = false;
        }
    if (d_geojson_output_enabled)
        {
            d_geojson_printer = std::make_unique<GeoJSON_Printer>(conf_.geojson_output_path);
            d_geojson_printer->set_headers(geojson_dump_filename);
        }
    else
        {
            d_geojson_printer = nullptr;
        }

    // initialize nmea_printer
    d_nmea_output_file_enabled = (conf_.nmea_output_file_enabled or conf_.flag_nmea_tty_port);
    d_nmea_rate_ms = conf_.nmea_rate_ms;
    if (d_nmea_rate_ms == 0)
        {
            d_nmea_output_file_enabled = false;
        }

    if (d_nmea_output_file_enabled)
        {
            d_nmea_printer = std::make_unique<Nmea_Printer>(conf_.nmea_dump_filename, conf_.nmea_output_file_enabled, conf_.flag_nmea_tty_port, conf_.nmea_dump_devname, conf_.nmea_output_file_path);
        }
    else
        {
            d_nmea_printer = nullptr;
        }

    // initialize rtcm_printer
    const std::string rtcm_dump_filename = d_dump_filename;
    if (conf_.flag_rtcm_server or conf_.flag_rtcm_tty_port or conf_.rtcm_output_file_enabled)
        {
            d_rtcm_printer = std::make_unique<Rtcm_Printer>(rtcm_dump_filename, conf_.rtcm_output_file_enabled, conf_.flag_rtcm_server, conf_.flag_rtcm_tty_port, conf_.rtcm_tcp_port, conf_.rtcm_station_id, conf_.rtcm_dump_devname, true, conf_.rtcm_output_file_path);
            std::map<int, int> rtcm_msg_rate_ms = conf_.rtcm_msg_rate_ms;
            if (rtcm_msg_rate_ms.find(1019) != rtcm_msg_rate_ms.end())
                {
                    d_rtcm_MT1019_rate_ms = rtcm_msg_rate_ms[1019];
                }
            else
                {
                    d_rtcm_MT1019_rate_ms = bc::lcm(5000, d_output_rate_ms);  // default value if not set
                }
            if (rtcm_msg_rate_ms.find(1020) != rtcm_msg_rate_ms.end())
                {
                    d_rtcm_MT1020_rate_ms = rtcm_msg_rate_ms[1020];
                }
            else
                {
                    d_rtcm_MT1020_rate_ms = bc::lcm(5000, d_output_rate_ms);  // default value if not set
                }
            if (rtcm_msg_rate_ms.find(1045) != rtcm_msg_rate_ms.end())
                {
                    d_rtcm_MT1045_rate_ms = rtcm_msg_rate_ms[1045];
                }
            else
                {
                    d_rtcm_MT1045_rate_ms = bc::lcm(5000, d_output_rate_ms);  // default value if not set
                }
            if (rtcm_msg_rate_ms.find(1077) != rtcm_msg_rate_ms.end())  // whatever between 1071 and 1077
                {
                    d_rtcm_MT1077_rate_ms = rtcm_msg_rate_ms[1077];
                }
            else
                {
                    d_rtcm_MT1077_rate_ms = bc::lcm(1000, d_output_rate_ms);  // default value if not set
                }
            if (rtcm_msg_rate_ms.find(1087) != rtcm_msg_rate_ms.end())  // whatever between 1081 and 1087
                {
                    d_rtcm_MT1087_rate_ms = rtcm_msg_rate_ms[1087];
                }
            else
                {
                    d_rtcm_MT1087_rate_ms = bc::lcm(1000, d_output_rate_ms);  // default value if not set
                }
            if (rtcm_msg_rate_ms.find(1097) != rtcm_msg_rate_ms.end())  // whatever between 1091 and 1097
                {
                    d_rtcm_MT1097_rate_ms = rtcm_msg_rate_ms[1097];
                    d_rtcm_MSM_rate_ms = rtcm_msg_rate_ms[1097];
                }
            else
                {
                    d_rtcm_MT1097_rate_ms = bc::lcm(1000, d_output_rate_ms);  // default value if not set
                    d_rtcm_MSM_rate_ms = bc::lcm(1000, d_output_rate_ms);     // default value if not set
                }
            d_rtcm_writing_started = false;
            d_rtcm_enabled = true;
        }
    else
        {
            d_rtcm_MT1019_rate_ms = 0;
            d_rtcm_MT1045_rate_ms = 0;
            d_rtcm_MT1020_rate_ms = 0;
            d_rtcm_MT1077_rate_ms = 0;
            d_rtcm_MT1087_rate_ms = 0;
            d_rtcm_MT1097_rate_ms = 0;
            d_rtcm_MSM_rate_ms = 0;
            d_rtcm_enabled = false;
            d_rtcm_writing_started = false;
            d_rtcm_printer = nullptr;
        }

    // initialize RINEX printer
    d_rinex_header_written = false;
    d_rinex_header_updated = false;
    d_rinex_output_enabled = conf_.rinex_output_enabled;
    d_rinex_version = conf_.rinex_version;
    if (d_rinex_output_enabled)
        {
            d_rp = std::make_unique<Rinex_Printer>(d_rinex_version, conf_.rinex_output_path, conf_.rinex_name);
            d_rp->set_pre_2009_file(conf_.pre_2009_file);
        }
    else
        {
            d_rp = nullptr;
        }
    d_rinexobs_rate_ms = conf_.rinexobs_rate_ms;

    // XML printer
    d_xml_storage = conf_.xml_output_enabled;
    if (d_xml_storage)
        {
            d_xml_base_path = conf_.xml_output_path;
            fs::path full_path(fs::current_path());
            const fs::path p(d_xml_base_path);
            if (!fs::exists(p))
                {
                    std::string new_folder;
                    for (auto& folder : fs::path(d_xml_base_path))
                        {
                            new_folder += folder.string();
                            errorlib::error_code ec;
                            if (!fs::exists(new_folder))
                                {
                                    if (!fs::create_directory(new_folder, ec))
                                        {
                                            std::cout << "Could not create the " << new_folder << " folder.\n";
                                            d_xml_base_path = full_path.string();
                                        }
                                }
                            new_folder += fs::path::preferred_separator;
                        }
                }
            else
                {
                    d_xml_base_path = p.string();
                }
            if (d_xml_base_path != ".")
                {
                    std::cout << "XML files will be stored at " << d_xml_base_path << '\n';
                }

            d_xml_base_path = d_xml_base_path + fs::path::preferred_separator;
        }

    d_rx_time = 0.0;
    d_last_status_print_seg = 0;

    // PVT MONITOR
    d_flag_monitor_pvt_enabled = conf_.monitor_enabled;
    if (d_flag_monitor_pvt_enabled)
        {
            std::string address_string = conf_.udp_addresses;
            std::vector<std::string> udp_addr_vec = split_string(address_string, '_');
            std::sort(udp_addr_vec.begin(), udp_addr_vec.end());
            udp_addr_vec.erase(std::unique(udp_addr_vec.begin(), udp_addr_vec.end()), udp_addr_vec.end());

            d_udp_sink_ptr = std::make_unique<Monitor_Pvt_Udp_Sink>(udp_addr_vec, conf_.udp_port, conf_.protobuf_enabled);
        }
    else
        {
            d_udp_sink_ptr = nullptr;
        }

    // Create Sys V message queue
    d_first_fix = true;
    d_sysv_msg_key = 1101;
    const int msgflg = IPC_CREAT | 0666;
    if ((d_sysv_msqid = msgget(d_sysv_msg_key, msgflg)) == -1)
        {
            std::cout << "GNSS-SDR cannot create System V message queues.\n";
            LOG(WARNING) << "The System V message queue is not available. Error: " << errno << " - " << strerror(errno);
        }

    // Display time in local time zone
    d_show_local_time_zone = conf_.show_local_time_zone;
    std::ostringstream os;
#ifdef HAS_PUT_TIME
    time_t when = std::time(nullptr);
    auto const tm = *std::localtime(&when);
    os << std::put_time(&tm, "%z");
#endif
    std::string utc_diff_str = os.str();  // in ISO 8601 format: "+HHMM" or "-HHMM"
    if (utc_diff_str.empty())
        {
            utc_diff_str = "+0000";
        }
    const int h = std::stoi(utc_diff_str.substr(0, 3), nullptr, 10);
    const int m = std::stoi(utc_diff_str[0] + utc_diff_str.substr(3), nullptr, 10);
    d_utc_diff_time = boost::posix_time::hours(h) + boost::posix_time::minutes(m);
    std::ostringstream os2;
#ifdef HAS_PUT_TIME
    os2 << std::put_time(&tm, "%Z");
#endif
    const std::string time_zone_abrv = os2.str();
    if (time_zone_abrv.empty())
        {
            if (utc_diff_str == "+0000")
                {
                    d_local_time_str = " UTC";
                }
            else
                {
                    d_local_time_str = " (UTC " + utc_diff_str.substr(0, 3) + ":" + utc_diff_str.substr(3, 2) + ")";
                }
        }
    else
        {
            d_local_time_str = std::string(" ") + time_zone_abrv + " (UTC " + utc_diff_str.substr(0, 3) + ":" + utc_diff_str.substr(3, 2) + ")";
        }

    d_waiting_obs_block_rx_clock_offset_correction_msg = false;

    d_enable_rx_clock_correction = conf_.enable_rx_clock_correction;

    if (d_enable_rx_clock_correction == true)
        {
            // setup two PVT solvers: internal solver for rx clock and user solver
            // user PVT solver
            d_user_pvt_solver = std::make_shared<Rtklib_Solver>(rtk, static_cast<int32_t>(nchannels), dump_ls_pvt_filename, d_dump, d_dump_mat);
            d_user_pvt_solver->set_averaging_depth(1);
            d_user_pvt_solver->set_pre_2009_file(conf_.pre_2009_file);

            // internal PVT solver, mainly used to estimate the receiver clock
            rtk_t internal_rtk = rtk;
            internal_rtk.opt.mode = PMODE_SINGLE;  // use single positioning mode in internal PVT solver
            d_internal_pvt_solver = std::make_shared<Rtklib_Solver>(internal_rtk, static_cast<int32_t>(nchannels), dump_ls_pvt_filename, false, false);
            d_internal_pvt_solver->set_averaging_depth(1);
            d_internal_pvt_solver->set_pre_2009_file(conf_.pre_2009_file);
        }
    else
        {
            // only one solver, customized by the user options
            d_internal_pvt_solver = std::make_shared<Rtklib_Solver>(rtk, static_cast<int32_t>(nchannels), dump_ls_pvt_filename, d_dump, d_dump_mat);
            d_internal_pvt_solver->set_averaging_depth(1);
            d_internal_pvt_solver->set_pre_2009_file(conf_.pre_2009_file);
            d_user_pvt_solver = d_internal_pvt_solver;
        }

    d_gps_ephemeris_sptr_type_hash_code = typeid(std::shared_ptr<Gps_Ephemeris>).hash_code();
    d_gps_iono_sptr_type_hash_code = typeid(std::shared_ptr<Gps_Iono>).hash_code();
    d_gps_utc_model_sptr_type_hash_code = typeid(std::shared_ptr<Gps_Utc_Model>).hash_code();
    d_gps_cnav_ephemeris_sptr_type_hash_code = typeid(std::shared_ptr<Gps_CNAV_Ephemeris>).hash_code();
    d_gps_cnav_iono_sptr_type_hash_code = typeid(std::shared_ptr<Gps_CNAV_Iono>).hash_code();
    d_gps_cnav_utc_model_sptr_type_hash_code = typeid(std::shared_ptr<Gps_CNAV_Utc_Model>).hash_code();
    d_gps_almanac_sptr_type_hash_code = typeid(std::shared_ptr<Gps_Almanac>).hash_code();
    d_galileo_ephemeris_sptr_type_hash_code = typeid(std::shared_ptr<Galileo_Ephemeris>).hash_code();
    d_galileo_iono_sptr_type_hash_code = typeid(std::shared_ptr<Galileo_Iono>).hash_code();
    d_galileo_utc_model_sptr_type_hash_code = typeid(std::shared_ptr<Galileo_Utc_Model>).hash_code();
    d_galileo_almanac_helper_sptr_type_hash_code = typeid(std::shared_ptr<Galileo_Almanac_Helper>).hash_code();
    d_galileo_almanac_sptr_type_hash_code = typeid(std::shared_ptr<Galileo_Almanac>).hash_code();
    d_glonass_gnav_ephemeris_sptr_type_hash_code = typeid(std::shared_ptr<Glonass_Gnav_Ephemeris>).hash_code();
    d_glonass_gnav_utc_model_sptr_type_hash_code = typeid(std::shared_ptr<Glonass_Gnav_Utc_Model>).hash_code();
    d_glonass_gnav_almanac_sptr_type_hash_code = typeid(std::shared_ptr<Glonass_Gnav_Almanac>).hash_code();
    d_beidou_dnav_ephemeris_sptr_type_hash_code = typeid(std::shared_ptr<Beidou_Dnav_Ephemeris>).hash_code();
    d_beidou_dnav_iono_sptr_type_hash_code = typeid(std::shared_ptr<Beidou_Dnav_Iono>).hash_code();
    d_beidou_dnav_utc_model_sptr_type_hash_code = typeid(std::shared_ptr<Beidou_Dnav_Utc_Model>).hash_code();
    d_beidou_dnav_almanac_sptr_type_hash_code = typeid(std::shared_ptr<Beidou_Dnav_Almanac>).hash_code();

    d_start = std::chrono::system_clock::now();
}


rtklib_pvt_gs::~rtklib_pvt_gs()
{
    DLOG(INFO) << "PVT block destructor called.";
    if (d_PPS_correction)
        {
            vxi11_close_device(d_clink, d_SMA_IP_address.c_str());
        }
    if (d_sysv_msqid != -1)
        {
            msgctl(d_sysv_msqid, IPC_RMID, nullptr);
        }
    try
        {
            if (d_xml_storage)
                {
                    // save GPS L2CM ephemeris to XML file
                    std::string file_name = d_xml_base_path + "gps_cnav_ephemeris.xml";
                    if (d_internal_pvt_solver->gps_cnav_ephemeris_map.empty() == false)
                        {
                            std::ofstream ofs;
                            try
                                {
                                    ofs.open(file_name.c_str(), std::ofstream::trunc | std::ofstream::out);
                                    boost::archive::xml_oarchive xml(ofs);
                                    xml << boost::serialization::make_nvp("GNSS-SDR_cnav_ephemeris_map", d_internal_pvt_solver->gps_cnav_ephemeris_map);
                                    LOG(INFO) << "Saved GPS L2CM or L5 Ephemeris map data";
                                }
                            catch (const boost::archive::archive_exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                            catch (const std::exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                        }
                    else
                        {
                            LOG(INFO) << "Failed to save GPS L2CM or L5 Ephemeris, map is empty";
                        }

                    // save GPS L1 CA ephemeris to XML file
                    file_name = d_xml_base_path + "gps_ephemeris.xml";
                    if (d_internal_pvt_solver->gps_ephemeris_map.empty() == false)
                        {
                            std::ofstream ofs;
                            try
                                {
                                    ofs.open(file_name.c_str(), std::ofstream::trunc | std::ofstream::out);
                                    boost::archive::xml_oarchive xml(ofs);
                                    xml << boost::serialization::make_nvp("GNSS-SDR_ephemeris_map", d_internal_pvt_solver->gps_ephemeris_map);
                                    LOG(INFO) << "Saved GPS L1 CA Ephemeris map data";
                                }
                            catch (const boost::archive::archive_exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                            catch (const std::exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                        }
                    else
                        {
                            LOG(INFO) << "Failed to save GPS L1 CA Ephemeris, map is empty";
                        }

                    // save Galileo E1 ephemeris to XML file
                    file_name = d_xml_base_path + "gal_ephemeris.xml";
                    if (d_internal_pvt_solver->galileo_ephemeris_map.empty() == false)
                        {
                            std::ofstream ofs;
                            try
                                {
                                    ofs.open(file_name.c_str(), std::ofstream::trunc | std::ofstream::out);
                                    boost::archive::xml_oarchive xml(ofs);
                                    xml << boost::serialization::make_nvp("GNSS-SDR_gal_ephemeris_map", d_internal_pvt_solver->galileo_ephemeris_map);
                                    LOG(INFO) << "Saved Galileo E1 Ephemeris map data";
                                }
                            catch (const boost::archive::archive_exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                            catch (const std::ofstream::failure& e)
                                {
                                    LOG(WARNING) << "Problem opening output XML file";
                                }
                            catch (const std::exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                        }
                    else
                        {
                            LOG(INFO) << "Failed to save Galileo E1 Ephemeris, map is empty";
                        }

                    // save GLONASS GNAV ephemeris to XML file
                    file_name = d_xml_base_path + "eph_GLONASS_GNAV.xml";
                    if (d_internal_pvt_solver->glonass_gnav_ephemeris_map.empty() == false)
                        {
                            std::ofstream ofs;
                            try
                                {
                                    ofs.open(file_name.c_str(), std::ofstream::trunc | std::ofstream::out);
                                    boost::archive::xml_oarchive xml(ofs);
                                    xml << boost::serialization::make_nvp("GNSS-SDR_gnav_ephemeris_map", d_internal_pvt_solver->glonass_gnav_ephemeris_map);
                                    LOG(INFO) << "Saved GLONASS GNAV Ephemeris map data";
                                }
                            catch (const boost::archive::archive_exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                            catch (const std::ofstream::failure& e)
                                {
                                    LOG(WARNING) << "Problem opening output XML file";
                                }
                            catch (const std::exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                        }
                    else
                        {
                            LOG(INFO) << "Failed to save GLONASS GNAV Ephemeris, map is empty";
                        }

                    // Save GPS UTC model parameters
                    file_name = d_xml_base_path + "gps_utc_model.xml";
                    if (d_internal_pvt_solver->gps_utc_model.valid)
                        {
                            std::ofstream ofs;
                            try
                                {
                                    ofs.open(file_name.c_str(), std::ofstream::trunc | std::ofstream::out);
                                    boost::archive::xml_oarchive xml(ofs);
                                    xml << boost::serialization::make_nvp("GNSS-SDR_utc_model", d_internal_pvt_solver->gps_utc_model);
                                    LOG(INFO) << "Saved GPS UTC model parameters";
                                }
                            catch (const std::ofstream::failure& e)
                                {
                                    LOG(WARNING) << "Problem opening output XML file";
                                }
                            catch (const boost::archive::archive_exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                            catch (const std::exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                        }
                    else
                        {
                            LOG(INFO) << "Failed to save GPS UTC model parameters, not valid data";
                        }

                    // Save Galileo UTC model parameters
                    file_name = d_xml_base_path + "gal_utc_model.xml";
                    if (d_internal_pvt_solver->galileo_utc_model.Delta_tLS_6 != 0.0)
                        {
                            std::ofstream ofs;
                            try
                                {
                                    ofs.open(file_name.c_str(), std::ofstream::trunc | std::ofstream::out);
                                    boost::archive::xml_oarchive xml(ofs);
                                    xml << boost::serialization::make_nvp("GNSS-SDR_gal_utc_model", d_internal_pvt_solver->galileo_utc_model);
                                    LOG(INFO) << "Saved Galileo UTC model parameters";
                                }
                            catch (const boost::archive::archive_exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                            catch (const std::ofstream::failure& e)
                                {
                                    LOG(WARNING) << "Problem opening output XML file";
                                }
                            catch (const std::exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                        }
                    else
                        {
                            LOG(INFO) << "Failed to save Galileo UTC model parameters, not valid data";
                        }

                    // Save GPS iono parameters
                    file_name = d_xml_base_path + "gps_iono.xml";
                    if (d_internal_pvt_solver->gps_iono.valid == true)
                        {
                            std::ofstream ofs;
                            try
                                {
                                    ofs.open(file_name.c_str(), std::ofstream::trunc | std::ofstream::out);
                                    boost::archive::xml_oarchive xml(ofs);
                                    xml << boost::serialization::make_nvp("GNSS-SDR_iono_model", d_internal_pvt_solver->gps_iono);
                                    LOG(INFO) << "Saved GPS ionospheric model parameters";
                                }
                            catch (const boost::archive::archive_exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                            catch (const std::ofstream::failure& e)
                                {
                                    LOG(WARNING) << "Problem opening output XML file";
                                }
                            catch (const std::exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                        }
                    else
                        {
                            LOG(INFO) << "Failed to save GPS ionospheric model parameters, not valid data";
                        }

                    // Save GPS CNAV iono parameters
                    file_name = d_xml_base_path + "gps_cnav_iono.xml";
                    if (d_internal_pvt_solver->gps_cnav_iono.valid == true)
                        {
                            std::ofstream ofs;
                            try
                                {
                                    ofs.open(file_name.c_str(), std::ofstream::trunc | std::ofstream::out);
                                    boost::archive::xml_oarchive xml(ofs);
                                    xml << boost::serialization::make_nvp("GNSS-SDR_cnav_iono_model", d_internal_pvt_solver->gps_cnav_iono);
                                    LOG(INFO) << "Saved GPS CNAV ionospheric model parameters";
                                }
                            catch (const boost::archive::archive_exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                            catch (const std::ofstream::failure& e)
                                {
                                    LOG(WARNING) << "Problem opening output XML file";
                                }
                            catch (const std::exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                        }
                    else
                        {
                            LOG(INFO) << "Failed to save GPS CNAV ionospheric model parameters, not valid data";
                        }

                    // Save Galileo iono parameters
                    file_name = d_xml_base_path + "gal_iono.xml";
                    if (d_internal_pvt_solver->galileo_iono.ai0_5 != 0.0)
                        {
                            std::ofstream ofs;
                            try
                                {
                                    ofs.open(file_name.c_str(), std::ofstream::trunc | std::ofstream::out);
                                    boost::archive::xml_oarchive xml(ofs);
                                    xml << boost::serialization::make_nvp("GNSS-SDR_gal_iono_model", d_internal_pvt_solver->galileo_iono);
                                    LOG(INFO) << "Saved Galileo ionospheric model parameters";
                                }
                            catch (const boost::archive::archive_exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                            catch (const std::ofstream::failure& e)
                                {
                                    LOG(WARNING) << "Problem opening output XML file";
                                }
                            catch (const std::exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                        }
                    else
                        {
                            LOG(INFO) << "Failed to save Galileo ionospheric model parameters, not valid data";
                        }

                    // save GPS almanac to XML file
                    file_name = d_xml_base_path + "gps_almanac.xml";
                    if (d_internal_pvt_solver->gps_almanac_map.empty() == false)
                        {
                            std::ofstream ofs;
                            try
                                {
                                    ofs.open(file_name.c_str(), std::ofstream::trunc | std::ofstream::out);
                                    boost::archive::xml_oarchive xml(ofs);
                                    xml << boost::serialization::make_nvp("GNSS-SDR_gps_almanac_map", d_internal_pvt_solver->gps_almanac_map);
                                    LOG(INFO) << "Saved GPS almanac map data";
                                }
                            catch (const boost::archive::archive_exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                            catch (const std::ofstream::failure& e)
                                {
                                    LOG(WARNING) << "Problem opening output XML file";
                                }
                            catch (const std::exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                        }
                    else
                        {
                            LOG(INFO) << "Failed to save GPS almanac, map is empty";
                        }

                    // Save Galileo almanac
                    file_name = d_xml_base_path + "gal_almanac.xml";
                    if (d_internal_pvt_solver->galileo_almanac_map.empty() == false)
                        {
                            std::ofstream ofs;
                            try
                                {
                                    ofs.open(file_name.c_str(), std::ofstream::trunc | std::ofstream::out);
                                    boost::archive::xml_oarchive xml(ofs);
                                    xml << boost::serialization::make_nvp("GNSS-SDR_gal_almanac_map", d_internal_pvt_solver->galileo_almanac_map);
                                    LOG(INFO) << "Saved Galileo almanac data";
                                }
                            catch (const boost::archive::archive_exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                            catch (const std::ofstream::failure& e)
                                {
                                    LOG(WARNING) << "Problem opening output XML file";
                                }
                            catch (const std::exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                        }
                    else
                        {
                            LOG(INFO) << "Failed to save Galileo almanac, not valid data";
                        }

                    // Save GPS CNAV UTC model parameters
                    file_name = d_xml_base_path + "gps_cnav_utc_model.xml";
                    if (d_internal_pvt_solver->gps_cnav_utc_model.valid)
                        {
                            std::ofstream ofs;
                            try
                                {
                                    ofs.open(file_name.c_str(), std::ofstream::trunc | std::ofstream::out);
                                    boost::archive::xml_oarchive xml(ofs);
                                    xml << boost::serialization::make_nvp("GNSS-SDR_cnav_utc_model", d_internal_pvt_solver->gps_cnav_utc_model);
                                    LOG(INFO) << "Saved GPS CNAV UTC model parameters";
                                }
                            catch (const boost::archive::archive_exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                            catch (const std::ofstream::failure& e)
                                {
                                    LOG(WARNING) << "Problem opening output XML file";
                                }
                            catch (const std::exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                        }
                    else
                        {
                            LOG(INFO) << "Failed to save GPS CNAV UTC model parameters, not valid data";
                        }

                    // save GLONASS GNAV ephemeris to XML file
                    file_name = d_xml_base_path + "glo_gnav_ephemeris.xml";
                    if (d_internal_pvt_solver->glonass_gnav_ephemeris_map.empty() == false)
                        {
                            std::ofstream ofs;
                            try
                                {
                                    ofs.open(file_name.c_str(), std::ofstream::trunc | std::ofstream::out);
                                    boost::archive::xml_oarchive xml(ofs);
                                    xml << boost::serialization::make_nvp("GNSS-SDR_gnav_ephemeris_map", d_internal_pvt_solver->glonass_gnav_ephemeris_map);
                                    LOG(INFO) << "Saved GLONASS GNAV ephemeris map data";
                                }
                            catch (const boost::archive::archive_exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                            catch (const std::ofstream::failure& e)
                                {
                                    LOG(WARNING) << "Problem opening output XML file";
                                }
                            catch (const std::exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                        }
                    else
                        {
                            LOG(INFO) << "Failed to save GLONASS GNAV ephemeris, map is empty";
                        }

                    // save GLONASS UTC model parameters to XML file
                    file_name = d_xml_base_path + "glo_utc_model.xml";
                    if (d_internal_pvt_solver->glonass_gnav_utc_model.valid)
                        {
                            std::ofstream ofs;
                            try
                                {
                                    ofs.open(file_name.c_str(), std::ofstream::trunc | std::ofstream::out);
                                    boost::archive::xml_oarchive xml(ofs);
                                    xml << boost::serialization::make_nvp("GNSS-SDR_gnav_utc_model", d_internal_pvt_solver->glonass_gnav_utc_model);
                                    LOG(INFO) << "Saved GLONASS UTC model parameters";
                                }
                            catch (const boost::archive::archive_exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                            catch (const std::ofstream::failure& e)
                                {
                                    LOG(WARNING) << "Problem opening output XML file";
                                }
                            catch (const std::exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                        }
                    else
                        {
                            LOG(INFO) << "Failed to save GLONASS GNAV ephemeris, not valid data";
                        }

                    // save BeiDou DNAV ephemeris to XML file
                    file_name = d_xml_base_path + "bds_dnav_ephemeris.xml";
                    if (d_internal_pvt_solver->beidou_dnav_ephemeris_map.empty() == false)
                        {
                            std::ofstream ofs;
                            try
                                {
                                    ofs.open(file_name.c_str(), std::ofstream::trunc | std::ofstream::out);
                                    boost::archive::xml_oarchive xml(ofs);
                                    xml << boost::serialization::make_nvp("GNSS-SDR_bds_dnav_ephemeris_map", d_internal_pvt_solver->beidou_dnav_ephemeris_map);
                                    LOG(INFO) << "Saved BeiDou DNAV Ephemeris map data";
                                }
                            catch (const boost::archive::archive_exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                            catch (const std::ofstream::failure& e)
                                {
                                    LOG(WARNING) << "Problem opening output XML file";
                                }
                            catch (const std::exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                        }
                    else
                        {
                            LOG(INFO) << "Failed to save BeiDou DNAV Ephemeris, map is empty";
                        }

                    // Save BeiDou DNAV iono parameters
                    file_name = d_xml_base_path + "bds_dnav_iono.xml";
                    if (d_internal_pvt_solver->beidou_dnav_iono.valid)
                        {
                            std::ofstream ofs;
                            try
                                {
                                    ofs.open(file_name.c_str(), std::ofstream::trunc | std::ofstream::out);
                                    boost::archive::xml_oarchive xml(ofs);
                                    xml << boost::serialization::make_nvp("GNSS-SDR_bds_dnav_iono_model", d_internal_pvt_solver->beidou_dnav_iono);
                                    LOG(INFO) << "Saved BeiDou DNAV ionospheric model parameters";
                                }
                            catch (const boost::archive::archive_exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                            catch (const std::ofstream::failure& e)
                                {
                                    LOG(WARNING) << "Problem opening output XML file";
                                }
                            catch (const std::exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                        }
                    else
                        {
                            LOG(INFO) << "Failed to save BeiDou DNAV ionospheric model parameters, not valid data";
                        }

                    // save BeiDou DNAV almanac to XML file
                    file_name = d_xml_base_path + "bds_dnav_almanac.xml";
                    if (d_internal_pvt_solver->beidou_dnav_almanac_map.empty() == false)
                        {
                            std::ofstream ofs;
                            try
                                {
                                    ofs.open(file_name.c_str(), std::ofstream::trunc | std::ofstream::out);
                                    boost::archive::xml_oarchive xml(ofs);
                                    xml << boost::serialization::make_nvp("GNSS-SDR_bds_dnav_almanac_map", d_internal_pvt_solver->beidou_dnav_almanac_map);
                                    LOG(INFO) << "Saved BeiDou DNAV almanac map data";
                                }
                            catch (const boost::archive::archive_exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                            catch (const std::ofstream::failure& e)
                                {
                                    LOG(WARNING) << "Problem opening output XML file";
                                }
                            catch (const std::exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                        }
                    else
                        {
                            LOG(INFO) << "Failed to save BeiDou DNAV almanac, map is empty";
                        }

                    // Save BeiDou UTC model parameters
                    file_name = d_xml_base_path + "bds_dnav_utc_model.xml";
                    if (d_internal_pvt_solver->beidou_dnav_utc_model.valid)
                        {
                            std::ofstream ofs;
                            try
                                {
                                    ofs.open(file_name.c_str(), std::ofstream::trunc | std::ofstream::out);
                                    boost::archive::xml_oarchive xml(ofs);
                                    xml << boost::serialization::make_nvp("GNSS-SDR_bds_dnav_utc_model", d_internal_pvt_solver->beidou_dnav_utc_model);
                                    LOG(INFO) << "Saved BeiDou DNAV UTC model parameters";
                                }
                            catch (const boost::archive::archive_exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                            catch (const std::ofstream::failure& e)
                                {
                                    LOG(WARNING) << "Problem opening output XML file";
                                }
                            catch (const std::exception& e)
                                {
                                    LOG(WARNING) << e.what();
                                }
                        }
                    else
                        {
                            LOG(INFO) << "Failed to save BeiDou DNAV UTC model parameters, not valid data";
                        }
                }
        }
    catch (std::length_error& e)
        {
            LOG(WARNING) << e.what();
        }
}


void rtklib_pvt_gs::msg_handler_telemetry(const pmt::pmt_t& msg)
{
    try
        {
            const size_t msg_type_hash_code = pmt::any_ref(msg).type().hash_code();
            // ************************* GPS telemetry *************************
            if (msg_type_hash_code == d_gps_ephemeris_sptr_type_hash_code)
                {
                    // ### GPS EPHEMERIS ###
                    const std::shared_ptr<Gps_Ephemeris> gps_eph = boost::any_cast<std::shared_ptr<Gps_Ephemeris>>(pmt::any_ref(msg));
                    DLOG(INFO) << "Ephemeris record has arrived from SAT ID "
                               << gps_eph->i_satellite_PRN << " (Block "
                               << gps_eph->satelliteBlock[gps_eph->i_satellite_PRN] << ")"
                               << "inserted with Toe=" << gps_eph->d_Toe << " and GPS Week="
                               << gps_eph->i_GPS_week;
                    // update/insert new ephemeris record to the global ephemeris map
                    if (d_rinex_header_written)  // The header is already written, we can now log the navigation message data
                        {
                            bool new_annotation = false;
                            if (d_internal_pvt_solver->gps_ephemeris_map.find(gps_eph->i_satellite_PRN) == d_internal_pvt_solver->gps_ephemeris_map.cend())
                                {
                                    new_annotation = true;
                                }
                            else
                                {
                                    if (d_internal_pvt_solver->gps_ephemeris_map[gps_eph->i_satellite_PRN].d_Toe != gps_eph->d_Toe)
                                        {
                                            new_annotation = true;
                                        }
                                }
                            if (new_annotation == true)
                                {
                                    // New record!
                                    std::map<int32_t, Gps_Ephemeris> new_eph;
                                    std::map<int32_t, Galileo_Ephemeris> new_gal_eph;
                                    std::map<int32_t, Glonass_Gnav_Ephemeris> new_glo_eph;
                                    new_eph[gps_eph->i_satellite_PRN] = *gps_eph;
                                    switch (d_type_of_rx)
                                        {
                                        case 1:  // GPS L1 C/A only
                                        case 8:  // L1+L5
                                            d_rp->log_rinex_nav(d_rp->navFile, new_eph);
                                            break;
                                        case 9:   // GPS L1 C/A + Galileo E1B
                                        case 10:  // GPS L1 C/A + Galileo E5a
                                        case 11:  // GPS L1 C/A + Galileo E5b
                                            d_rp->log_rinex_nav(d_rp->navMixFile, new_eph, new_gal_eph);
                                            break;
                                        case 26:  // GPS L1 C/A + GLONASS L1 C/A
                                            if (d_rinex_version == 3)
                                                {
                                                    d_rp->log_rinex_nav(d_rp->navMixFile, new_eph, new_glo_eph);
                                                }
                                            if (d_rinex_version == 2)
                                                {
                                                    d_rp->log_rinex_nav(d_rp->navFile, new_glo_eph);
                                                }
                                            break;
                                        case 29:  // GPS L1 C/A + GLONASS L2 C/A
                                            if (d_rinex_version == 3)
                                                {
                                                    d_rp->log_rinex_nav(d_rp->navMixFile, new_eph, new_glo_eph);
                                                }
                                            if (d_rinex_version == 2)
                                                {
                                                    d_rp->log_rinex_nav(d_rp->navFile, new_eph);
                                                }
                                            break;
                                        case 32:  // L1+E1+L5+E5a
                                        case 33:  // L1+E1+E5a
                                            d_rp->log_rinex_nav(d_rp->navMixFile, new_eph, new_gal_eph);
                                            break;
                                        case 1000:  // L1+L2+L5
                                            d_rp->log_rinex_nav(d_rp->navFile, new_eph);
                                            break;
                                        case 1001:  // L1+E1+L2+L5+E5a
                                            d_rp->log_rinex_nav(d_rp->navMixFile, new_eph, new_gal_eph);
                                            break;
                                        default:
                                            break;
                                        }
                                }
                        }
                    d_internal_pvt_solver->gps_ephemeris_map[gps_eph->i_satellite_PRN] = *gps_eph;
                    if (d_enable_rx_clock_correction == true)
                        {
                            d_user_pvt_solver->gps_ephemeris_map[gps_eph->i_satellite_PRN] = *gps_eph;
                        }
                }
            else if (msg_type_hash_code == d_gps_iono_sptr_type_hash_code)
                {
                    // ### GPS IONO ###
                    const std::shared_ptr<Gps_Iono> gps_iono = boost::any_cast<std::shared_ptr<Gps_Iono>>(pmt::any_ref(msg));
                    d_internal_pvt_solver->gps_iono = *gps_iono;
                    if (d_enable_rx_clock_correction == true)
                        {
                            d_user_pvt_solver->gps_iono = *gps_iono;
                        }
                    DLOG(INFO) << "New IONO record has arrived ";
                }
            else if (msg_type_hash_code == d_gps_utc_model_sptr_type_hash_code)
                {
                    // ### GPS UTC MODEL ###
                    const std::shared_ptr<Gps_Utc_Model> gps_utc_model = boost::any_cast<std::shared_ptr<Gps_Utc_Model>>(pmt::any_ref(msg));
                    d_internal_pvt_solver->gps_utc_model = *gps_utc_model;
                    if (d_enable_rx_clock_correction == true)
                        {
                            d_user_pvt_solver->gps_utc_model = *gps_utc_model;
                        }
                    DLOG(INFO) << "New UTC record has arrived ";
                }
            else if (msg_type_hash_code == d_gps_cnav_ephemeris_sptr_type_hash_code)
                {
                    // ### GPS CNAV message ###
                    const std::shared_ptr<Gps_CNAV_Ephemeris> gps_cnav_ephemeris = boost::any_cast<std::shared_ptr<Gps_CNAV_Ephemeris>>(pmt::any_ref(msg));
                    // update/insert new ephemeris record to the global ephemeris map
                    if (d_rinex_header_written)  // The header is already written, we can now log the navigation message data
                        {
                            bool new_annotation = false;
                            if (d_internal_pvt_solver->gps_cnav_ephemeris_map.find(gps_cnav_ephemeris->i_satellite_PRN) == d_internal_pvt_solver->gps_cnav_ephemeris_map.cend())
                                {
                                    new_annotation = true;
                                }
                            else
                                {
                                    if (d_internal_pvt_solver->gps_cnav_ephemeris_map[gps_cnav_ephemeris->i_satellite_PRN].d_Toe1 != gps_cnav_ephemeris->d_Toe1)
                                        {
                                            new_annotation = true;
                                        }
                                }
                            if (new_annotation == true)
                                {
                                    // New record!
                                    std::map<int32_t, Galileo_Ephemeris> new_gal_eph;
                                    std::map<int32_t, Gps_CNAV_Ephemeris> new_cnav_eph;
                                    std::map<int32_t, Glonass_Gnav_Ephemeris> new_glo_eph;
                                    new_cnav_eph[gps_cnav_ephemeris->i_satellite_PRN] = *gps_cnav_ephemeris;
                                    switch (d_type_of_rx)
                                        {
                                        case 2:  // GPS L2C only
                                        case 3:  // GPS L5 only
                                        case 7:  // GPS L1 C/A + GPS L2C
                                            d_rp->log_rinex_nav(d_rp->navFile, new_cnav_eph);
                                            break;
                                        case 13:  // L5+E5a
                                            d_rp->log_rinex_nav(d_rp->navMixFile, new_cnav_eph, new_gal_eph);
                                            break;
                                        case 28:  // GPS L2C + GLONASS L1 C/A
                                        case 31:  // GPS L2C + GLONASS L2 C/A
                                            d_rp->log_rinex_nav(d_rp->navMixFile, new_cnav_eph, new_glo_eph);
                                            break;
                                        default:
                                            break;
                                        }
                                }
                        }
                    d_internal_pvt_solver->gps_cnav_ephemeris_map[gps_cnav_ephemeris->i_satellite_PRN] = *gps_cnav_ephemeris;
                    if (d_enable_rx_clock_correction == true)
                        {
                            d_user_pvt_solver->gps_cnav_ephemeris_map[gps_cnav_ephemeris->i_satellite_PRN] = *gps_cnav_ephemeris;
                        }
                    DLOG(INFO) << "New GPS CNAV ephemeris record has arrived ";
                }
            else if (msg_type_hash_code == d_gps_cnav_iono_sptr_type_hash_code)
                {
                    // ### GPS CNAV IONO ###
                    const std::shared_ptr<Gps_CNAV_Iono> gps_cnav_iono = boost::any_cast<std::shared_ptr<Gps_CNAV_Iono>>(pmt::any_ref(msg));
                    d_internal_pvt_solver->gps_cnav_iono = *gps_cnav_iono;
                    if (d_enable_rx_clock_correction == true)
                        {
                            d_user_pvt_solver->gps_cnav_iono = *gps_cnav_iono;
                        }
                    DLOG(INFO) << "New CNAV IONO record has arrived ";
                }
            else if (msg_type_hash_code == d_gps_cnav_utc_model_sptr_type_hash_code)
                {
                    // ### GPS CNAV UTC MODEL ###
                    const std::shared_ptr<Gps_CNAV_Utc_Model> gps_cnav_utc_model = boost::any_cast<std::shared_ptr<Gps_CNAV_Utc_Model>>(pmt::any_ref(msg));
                    d_internal_pvt_solver->gps_cnav_utc_model = *gps_cnav_utc_model;
                    {
                        d_user_pvt_solver->gps_cnav_utc_model = *gps_cnav_utc_model;
                    }
                    DLOG(INFO) << "New CNAV UTC record has arrived ";
                }

            else if (msg_type_hash_code == d_gps_almanac_sptr_type_hash_code)
                {
                    // ### GPS ALMANAC ###
                    const std::shared_ptr<Gps_Almanac> gps_almanac = boost::any_cast<std::shared_ptr<Gps_Almanac>>(pmt::any_ref(msg));
                    d_internal_pvt_solver->gps_almanac_map[gps_almanac->i_satellite_PRN] = *gps_almanac;
                    if (d_enable_rx_clock_correction == true)
                        {
                            d_user_pvt_solver->gps_almanac_map[gps_almanac->i_satellite_PRN] = *gps_almanac;
                        }
                    DLOG(INFO) << "New GPS almanac record has arrived ";
                }

            // *********************** Galileo telemetry ***********************
            else if (msg_type_hash_code == d_galileo_ephemeris_sptr_type_hash_code)
                {
                    // ### Galileo EPHEMERIS ###
                    const std::shared_ptr<Galileo_Ephemeris> galileo_eph = boost::any_cast<std::shared_ptr<Galileo_Ephemeris>>(pmt::any_ref(msg));
                    // insert new ephemeris record
                    DLOG(INFO) << "Galileo New Ephemeris record inserted in global map with TOW =" << galileo_eph->TOW_5
                               << ", GALILEO Week Number =" << galileo_eph->WN_5
                               << " and Ephemeris IOD = " << galileo_eph->IOD_ephemeris;
                    // update/insert new ephemeris record to the global ephemeris map
                    if (d_rinex_header_written)  // The header is already written, we can now log the navigation message data
                        {
                            bool new_annotation = false;
                            if (d_internal_pvt_solver->galileo_ephemeris_map.find(galileo_eph->i_satellite_PRN) == d_internal_pvt_solver->galileo_ephemeris_map.cend())
                                {
                                    new_annotation = true;
                                }
                            else
                                {
                                    if (d_internal_pvt_solver->galileo_ephemeris_map[galileo_eph->i_satellite_PRN].t0e_1 != galileo_eph->t0e_1)
                                        {
                                            new_annotation = true;
                                        }
                                }
                            if (new_annotation == true)
                                {
                                    // New record!
                                    std::map<int32_t, Galileo_Ephemeris> new_gal_eph;
                                    std::map<int32_t, Gps_CNAV_Ephemeris> new_cnav_eph;
                                    std::map<int32_t, Gps_Ephemeris> new_eph;
                                    std::map<int32_t, Glonass_Gnav_Ephemeris> new_glo_eph;
                                    new_gal_eph[galileo_eph->i_satellite_PRN] = *galileo_eph;
                                    switch (d_type_of_rx)
                                        {
                                        case 4:  // Galileo E1B only
                                        case 5:  // Galileo E5a only
                                        case 6:  // Galileo E5b only
                                            d_rp->log_rinex_nav(d_rp->navGalFile, new_gal_eph);
                                            break;
                                        case 9:   // GPS L1 C/A + Galileo E1B
                                        case 10:  // GPS L1 C/A + Galileo E5a
                                        case 11:  // GPS L1 C/A + Galileo E5b
                                            d_rp->log_rinex_nav(d_rp->navMixFile, new_eph, new_gal_eph);
                                            break;
                                        case 13:  // L5+E5a
                                            d_rp->log_rinex_nav(d_rp->navMixFile, new_cnav_eph, new_gal_eph);
                                            break;
                                        case 15:  // Galileo E1B + Galileo E5b
                                            d_rp->log_rinex_nav(d_rp->navGalFile, new_gal_eph);
                                            break;
                                        case 27:  // Galileo E1B + GLONASS L1 C/A
                                        case 30:  // Galileo E1B + GLONASS L2 C/A
                                            d_rp->log_rinex_nav(d_rp->navMixFile, new_gal_eph, new_glo_eph);
                                            break;
                                        case 32:    // L1+E1+L5+E5a
                                        case 33:    // L1+E1+E5a
                                        case 1001:  // L1+E1+L2+L5+E5a
                                            d_rp->log_rinex_nav(d_rp->navMixFile, new_eph, new_gal_eph);
                                            break;
                                        default:
                                            break;
                                        }
                                }
                        }
                    d_internal_pvt_solver->galileo_ephemeris_map[galileo_eph->i_satellite_PRN] = *galileo_eph;
                    if (d_enable_rx_clock_correction == true)
                        {
                            d_user_pvt_solver->galileo_ephemeris_map[galileo_eph->i_satellite_PRN] = *galileo_eph;
                        }
                }
            else if (msg_type_hash_code == d_galileo_iono_sptr_type_hash_code)
                {
                    // ### Galileo IONO ###
                    const std::shared_ptr<Galileo_Iono> galileo_iono = boost::any_cast<std::shared_ptr<Galileo_Iono>>(pmt::any_ref(msg));
                    d_internal_pvt_solver->galileo_iono = *galileo_iono;
                    if (d_enable_rx_clock_correction == true)
                        {
                            d_user_pvt_solver->galileo_iono = *galileo_iono;
                        }
                    DLOG(INFO) << "New IONO record has arrived ";
                }
            else if (msg_type_hash_code == d_galileo_utc_model_sptr_type_hash_code)
                {
                    // ### Galileo UTC MODEL ###
                    const std::shared_ptr<Galileo_Utc_Model> galileo_utc_model = boost::any_cast<std::shared_ptr<Galileo_Utc_Model>>(pmt::any_ref(msg));
                    d_internal_pvt_solver->galileo_utc_model = *galileo_utc_model;
                    if (d_enable_rx_clock_correction == true)
                        {
                            d_user_pvt_solver->galileo_utc_model = *galileo_utc_model;
                        }
                    DLOG(INFO) << "New UTC record has arrived ";
                }
            else if (msg_type_hash_code == d_galileo_almanac_helper_sptr_type_hash_code)
                {
                    // ### Galileo Almanac ###
                    const std::shared_ptr<Galileo_Almanac_Helper> galileo_almanac_helper = boost::any_cast<std::shared_ptr<Galileo_Almanac_Helper>>(pmt::any_ref(msg));
                    const Galileo_Almanac sv1 = galileo_almanac_helper->get_almanac(1);
                    const Galileo_Almanac sv2 = galileo_almanac_helper->get_almanac(2);
                    const Galileo_Almanac sv3 = galileo_almanac_helper->get_almanac(3);

                    if (sv1.i_satellite_PRN != 0)
                        {
                            d_internal_pvt_solver->galileo_almanac_map[sv1.i_satellite_PRN] = sv1;
                            if (d_enable_rx_clock_correction == true)
                                {
                                    d_user_pvt_solver->galileo_almanac_map[sv1.i_satellite_PRN] = sv1;
                                }
                        }
                    if (sv2.i_satellite_PRN != 0)
                        {
                            d_internal_pvt_solver->galileo_almanac_map[sv2.i_satellite_PRN] = sv2;
                            if (d_enable_rx_clock_correction == true)
                                {
                                    d_user_pvt_solver->galileo_almanac_map[sv2.i_satellite_PRN] = sv2;
                                }
                        }
                    if (sv3.i_satellite_PRN != 0)
                        {
                            d_internal_pvt_solver->galileo_almanac_map[sv3.i_satellite_PRN] = sv3;
                            if (d_enable_rx_clock_correction == true)
                                {
                                    d_user_pvt_solver->galileo_almanac_map[sv3.i_satellite_PRN] = sv3;
                                }
                        }
                    DLOG(INFO) << "New Galileo Almanac data have arrived ";
                }
            else if (msg_type_hash_code == d_galileo_almanac_sptr_type_hash_code)
                {
                    // ### Galileo Almanac ###
                    const std::shared_ptr<Galileo_Almanac> galileo_alm = boost::any_cast<std::shared_ptr<Galileo_Almanac>>(pmt::any_ref(msg));
                    // update/insert new almanac record to the global almanac map
                    d_internal_pvt_solver->galileo_almanac_map[galileo_alm->i_satellite_PRN] = *galileo_alm;
                    if (d_enable_rx_clock_correction == true)
                        {
                            d_user_pvt_solver->galileo_almanac_map[galileo_alm->i_satellite_PRN] = *galileo_alm;
                        }
                }

            // **************** GLONASS GNAV Telemetry *************************
            else if (msg_type_hash_code == d_glonass_gnav_ephemeris_sptr_type_hash_code)
                {
                    // ### GLONASS GNAV EPHEMERIS ###
                    const std::shared_ptr<Glonass_Gnav_Ephemeris> glonass_gnav_eph = boost::any_cast<std::shared_ptr<Glonass_Gnav_Ephemeris>>(pmt::any_ref(msg));
                    // TODO Add GLONASS with gps week number and tow,
                    // insert new ephemeris record
                    DLOG(INFO) << "GLONASS GNAV New Ephemeris record inserted in global map with TOW =" << glonass_gnav_eph->d_TOW
                               << ", Week Number =" << glonass_gnav_eph->d_WN
                               << " and Ephemeris IOD in UTC = " << glonass_gnav_eph->compute_GLONASS_time(glonass_gnav_eph->d_t_b)
                               << " from SV = " << glonass_gnav_eph->i_satellite_slot_number;
                    // update/insert new ephemeris record to the global ephemeris map
                    if (d_rinex_header_written)  // The header is already written, we can now log the navigation message data
                        {
                            bool new_annotation = false;
                            if (d_internal_pvt_solver->glonass_gnav_ephemeris_map.find(glonass_gnav_eph->i_satellite_PRN) == d_internal_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                {
                                    new_annotation = true;
                                }
                            else
                                {
                                    if (d_internal_pvt_solver->glonass_gnav_ephemeris_map[glonass_gnav_eph->i_satellite_PRN].d_t_b != glonass_gnav_eph->d_t_b)
                                        {
                                            new_annotation = true;
                                        }
                                }
                            if (new_annotation == true)
                                {
                                    // New record!
                                    std::map<int32_t, Galileo_Ephemeris> new_gal_eph;
                                    std::map<int32_t, Gps_CNAV_Ephemeris> new_cnav_eph;
                                    std::map<int32_t, Gps_Ephemeris> new_eph;
                                    std::map<int32_t, Glonass_Gnav_Ephemeris> new_glo_eph;
                                    new_glo_eph[glonass_gnav_eph->i_satellite_PRN] = *glonass_gnav_eph;
                                    switch (d_type_of_rx)
                                        {
                                        case 23:  // GLONASS L1 C/A
                                        case 24:  // GLONASS L2 C/A
                                        case 25:  // GLONASS L1 C/A + GLONASS L2 C/A
                                            d_rp->log_rinex_nav(d_rp->navGloFile, new_glo_eph);
                                            break;
                                        case 26:  // GPS L1 C/A + GLONASS L1 C/A
                                            if (d_rinex_version == 3)
                                                {
                                                    d_rp->log_rinex_nav(d_rp->navMixFile, new_eph, new_glo_eph);
                                                }
                                            if (d_rinex_version == 2)
                                                {
                                                    d_rp->log_rinex_nav(d_rp->navGloFile, new_glo_eph);
                                                }
                                            break;
                                        case 27:  // Galileo E1B + GLONASS L1 C/A
                                            d_rp->log_rinex_nav(d_rp->navMixFile, new_gal_eph, new_glo_eph);
                                            break;
                                        case 28:  // GPS L2C + GLONASS L1 C/A
                                            d_rp->log_rinex_nav(d_rp->navMixFile, new_cnav_eph, new_glo_eph);
                                            break;
                                        case 29:  // GPS L1 C/A + GLONASS L2 C/A
                                            if (d_rinex_version == 3)
                                                {
                                                    d_rp->log_rinex_nav(d_rp->navMixFile, new_eph, new_glo_eph);
                                                }
                                            if (d_rinex_version == 2)
                                                {
                                                    d_rp->log_rinex_nav(d_rp->navGloFile, new_glo_eph);
                                                }
                                            break;
                                        case 30:  // Galileo E1B + GLONASS L2 C/A
                                            d_rp->log_rinex_nav(d_rp->navMixFile, new_gal_eph, new_glo_eph);
                                            break;
                                        case 31:  // GPS L2C + GLONASS L2 C/A
                                            d_rp->log_rinex_nav(d_rp->navMixFile, new_cnav_eph, new_glo_eph);
                                            break;
                                        default:
                                            break;
                                        }
                                }
                        }
                    d_internal_pvt_solver->glonass_gnav_ephemeris_map[glonass_gnav_eph->i_satellite_PRN] = *glonass_gnav_eph;
                    if (d_enable_rx_clock_correction == true)
                        {
                            d_user_pvt_solver->glonass_gnav_ephemeris_map[glonass_gnav_eph->i_satellite_PRN] = *glonass_gnav_eph;
                        }
                }
            else if (msg_type_hash_code == d_glonass_gnav_utc_model_sptr_type_hash_code)
                {
                    // ### GLONASS GNAV UTC MODEL ###
                    const std::shared_ptr<Glonass_Gnav_Utc_Model> glonass_gnav_utc_model = boost::any_cast<std::shared_ptr<Glonass_Gnav_Utc_Model>>(pmt::any_ref(msg));
                    d_internal_pvt_solver->glonass_gnav_utc_model = *glonass_gnav_utc_model;
                    if (d_enable_rx_clock_correction == true)
                        {
                            d_user_pvt_solver->glonass_gnav_utc_model = *glonass_gnav_utc_model;
                        }
                    DLOG(INFO) << "New GLONASS GNAV UTC record has arrived ";
                }
            else if (msg_type_hash_code == d_glonass_gnav_almanac_sptr_type_hash_code)
                {
                    // ### GLONASS GNAV Almanac ###
                    const std::shared_ptr<Glonass_Gnav_Almanac> glonass_gnav_almanac = boost::any_cast<std::shared_ptr<Glonass_Gnav_Almanac>>(pmt::any_ref(msg));
                    d_internal_pvt_solver->glonass_gnav_almanac = *glonass_gnav_almanac;
                    if (d_enable_rx_clock_correction == true)
                        {
                            d_user_pvt_solver->glonass_gnav_almanac = *glonass_gnav_almanac;
                        }
                    DLOG(INFO) << "New GLONASS GNAV Almanac has arrived "
                               << ", GLONASS GNAV Slot Number =" << glonass_gnav_almanac->d_n_A;
                }

            // *********************** BeiDou telemetry ************************
            else if (msg_type_hash_code == d_beidou_dnav_ephemeris_sptr_type_hash_code)
                {
                    // ### Beidou EPHEMERIS ###
                    const std::shared_ptr<Beidou_Dnav_Ephemeris> bds_dnav_eph = boost::any_cast<std::shared_ptr<Beidou_Dnav_Ephemeris>>(pmt::any_ref(msg));
                    DLOG(INFO) << "Ephemeris record has arrived from SAT ID "
                               << bds_dnav_eph->i_satellite_PRN << " (Block "
                               << bds_dnav_eph->satelliteBlock[bds_dnav_eph->i_satellite_PRN] << ")"
                               << "inserted with Toe=" << bds_dnav_eph->d_Toe << " and BDS Week="
                               << bds_dnav_eph->i_BEIDOU_week;
                    // update/insert new ephemeris record to the global ephemeris map
                    if (d_rinex_header_written)  // The header is already written, we can now log the navigation message data
                        {
                            bool new_annotation = false;
                            if (d_internal_pvt_solver->beidou_dnav_ephemeris_map.find(bds_dnav_eph->i_satellite_PRN) == d_internal_pvt_solver->beidou_dnav_ephemeris_map.cend())
                                {
                                    new_annotation = true;
                                }
                            else
                                {
                                    if (d_internal_pvt_solver->beidou_dnav_ephemeris_map[bds_dnav_eph->i_satellite_PRN].d_Toc != bds_dnav_eph->d_Toc)
                                        {
                                            new_annotation = true;
                                        }
                                }
                            if (new_annotation == true)
                                {
                                    // New record!
                                    std::map<int32_t, Beidou_Dnav_Ephemeris> new_bds_eph;
                                    new_bds_eph[bds_dnav_eph->i_satellite_PRN] = *bds_dnav_eph;
                                    switch (d_type_of_rx)
                                        {
                                        case 500:  // BDS B1I only
                                        case 600:  // BDS B3I only
                                            d_rp->log_rinex_nav(d_rp->navFile, new_bds_eph);
                                            break;
                                        default:
                                            break;
                                        }
                                }
                        }
                    d_internal_pvt_solver->beidou_dnav_ephemeris_map[bds_dnav_eph->i_satellite_PRN] = *bds_dnav_eph;
                    if (d_enable_rx_clock_correction == true)
                        {
                            d_user_pvt_solver->beidou_dnav_ephemeris_map[bds_dnav_eph->i_satellite_PRN] = *bds_dnav_eph;
                        }
                }
            else if (msg_type_hash_code == d_beidou_dnav_iono_sptr_type_hash_code)
                {
                    // ### BeiDou IONO ###
                    const std::shared_ptr<Beidou_Dnav_Iono> bds_dnav_iono = boost::any_cast<std::shared_ptr<Beidou_Dnav_Iono>>(pmt::any_ref(msg));
                    d_internal_pvt_solver->beidou_dnav_iono = *bds_dnav_iono;
                    if (d_enable_rx_clock_correction == true)
                        {
                            d_user_pvt_solver->beidou_dnav_iono = *bds_dnav_iono;
                        }
                    DLOG(INFO) << "New BeiDou DNAV IONO record has arrived ";
                }
            else if (msg_type_hash_code == d_beidou_dnav_utc_model_sptr_type_hash_code)
                {
                    // ### BeiDou UTC MODEL ###
                    const std::shared_ptr<Beidou_Dnav_Utc_Model> bds_dnav_utc_model = boost::any_cast<std::shared_ptr<Beidou_Dnav_Utc_Model>>(pmt::any_ref(msg));
                    d_internal_pvt_solver->beidou_dnav_utc_model = *bds_dnav_utc_model;
                    if (d_enable_rx_clock_correction == true)
                        {
                            d_user_pvt_solver->beidou_dnav_utc_model = *bds_dnav_utc_model;
                        }
                    DLOG(INFO) << "New BeiDou DNAV UTC record has arrived ";
                }
            else if (msg_type_hash_code == d_beidou_dnav_almanac_sptr_type_hash_code)
                {
                    // ### BeiDou ALMANAC ###
                    const std::shared_ptr<Beidou_Dnav_Almanac> bds_dnav_almanac = boost::any_cast<std::shared_ptr<Beidou_Dnav_Almanac>>(pmt::any_ref(msg));
                    d_internal_pvt_solver->beidou_dnav_almanac_map[bds_dnav_almanac->i_satellite_PRN] = *bds_dnav_almanac;
                    if (d_enable_rx_clock_correction == true)
                        {
                            d_user_pvt_solver->beidou_dnav_almanac_map[bds_dnav_almanac->i_satellite_PRN] = *bds_dnav_almanac;
                        }
                    DLOG(INFO) << "New BeiDou DNAV almanac record has arrived ";
                }
            else
                {
                    LOG(WARNING) << "msg_handler_telemetry unknown object type!";
                }
        }
    catch (boost::bad_any_cast& e)
        {
            LOG(WARNING) << "msg_handler_telemetry Bad any cast!";
        }
}


std::map<int, Gps_Ephemeris> rtklib_pvt_gs::get_gps_ephemeris_map() const
{
    return d_internal_pvt_solver->gps_ephemeris_map;
}


std::map<int, Gps_Almanac> rtklib_pvt_gs::get_gps_almanac_map() const
{
    return d_internal_pvt_solver->gps_almanac_map;
}


std::map<int, Galileo_Ephemeris> rtklib_pvt_gs::get_galileo_ephemeris_map() const
{
    return d_internal_pvt_solver->galileo_ephemeris_map;
}


std::map<int, Galileo_Almanac> rtklib_pvt_gs::get_galileo_almanac_map() const
{
    return d_internal_pvt_solver->galileo_almanac_map;
}


std::map<int, Beidou_Dnav_Ephemeris> rtklib_pvt_gs::get_beidou_dnav_ephemeris_map() const
{
    return d_internal_pvt_solver->beidou_dnav_ephemeris_map;
}


std::map<int, Beidou_Dnav_Almanac> rtklib_pvt_gs::get_beidou_dnav_almanac_map() const
{
    return d_internal_pvt_solver->beidou_dnav_almanac_map;
}


void rtklib_pvt_gs::clear_ephemeris()
{
    d_internal_pvt_solver->gps_ephemeris_map.clear();
    d_internal_pvt_solver->gps_almanac_map.clear();
    d_internal_pvt_solver->galileo_ephemeris_map.clear();
    d_internal_pvt_solver->galileo_almanac_map.clear();
    d_internal_pvt_solver->beidou_dnav_ephemeris_map.clear();
    d_internal_pvt_solver->beidou_dnav_almanac_map.clear();
    if (d_enable_rx_clock_correction == true)
        {
            d_user_pvt_solver->gps_ephemeris_map.clear();
            d_user_pvt_solver->gps_almanac_map.clear();
            d_user_pvt_solver->galileo_ephemeris_map.clear();
            d_user_pvt_solver->galileo_almanac_map.clear();
            d_user_pvt_solver->beidou_dnav_ephemeris_map.clear();
            d_user_pvt_solver->beidou_dnav_almanac_map.clear();
        }
}


bool rtklib_pvt_gs::send_sys_v_ttff_msg(d_ttff_msgbuf ttff)
{
    if (d_sysv_msqid != -1)
        {
            // Fill Sys V message structures
            int msgsend_size;
            d_ttff_msgbuf msg;
            msg.ttff = ttff.ttff;
            msgsend_size = sizeof(msg.ttff);
            msg.mtype = 1;  // default message ID

            // SEND SOLUTION OVER A MESSAGE QUEUE
            // non-blocking Sys V message send
            msgsnd(d_sysv_msqid, &msg, msgsend_size, IPC_NOWAIT);
            return true;
        }
    return false;
}


bool rtklib_pvt_gs::save_gnss_synchro_map_xml(const std::string& file_name)
{
    if (d_gnss_observables_map.empty() == false)
        {
            std::ofstream ofs;
            try
                {
                    ofs.open(file_name.c_str(), std::ofstream::trunc | std::ofstream::out);
                    boost::archive::xml_oarchive xml(ofs);
                    xml << boost::serialization::make_nvp("GNSS-SDR_gnss_synchro_map", d_gnss_observables_map);
                    LOG(INFO) << "Saved gnss_sychro map data";
                }
            catch (const std::exception& e)
                {
                    LOG(WARNING) << e.what();
                    return false;
                }
            return true;
        }

    LOG(WARNING) << "Failed to save gnss_synchro, map is empty";
    return false;
}


bool rtklib_pvt_gs::load_gnss_synchro_map_xml(const std::string& file_name)
{
    // load from xml (boost serialize)
    std::ifstream ifs;
    try
        {
            ifs.open(file_name.c_str(), std::ifstream::binary | std::ifstream::in);
            boost::archive::xml_iarchive xml(ifs);
            d_gnss_observables_map.clear();
            xml >> boost::serialization::make_nvp("GNSS-SDR_gnss_synchro_map", d_gnss_observables_map);
            // std::cout << "Loaded gnss_synchro map data with " << gnss_synchro_map.size() << " pseudoranges\n";
        }
    catch (const std::exception& e)
        {
            std::cout << e.what() << "File: " << file_name;
            return false;
        }
    return true;
}


std::vector<std::string> rtklib_pvt_gs::split_string(const std::string& s, char delim) const
{
    std::vector<std::string> v;
    std::stringstream ss(s);
    std::string item;

    while (std::getline(ss, item, delim))
        {
            *(std::back_inserter(v)++) = item;
        }

    return v;
}


bool rtklib_pvt_gs::get_latest_PVT(double* longitude_deg,
    double* latitude_deg,
    double* height_m,
    double* ground_speed_kmh,
    double* course_over_ground_deg,
    time_t* UTC_time) const
{
    if (d_enable_rx_clock_correction == true)
        {
            if (d_user_pvt_solver->is_valid_position())
                {
                    *latitude_deg = d_user_pvt_solver->get_latitude();
                    *longitude_deg = d_user_pvt_solver->get_longitude();
                    *height_m = d_user_pvt_solver->get_height();
                    *ground_speed_kmh = d_user_pvt_solver->get_speed_over_ground() * 3600.0 / 1000.0;
                    *course_over_ground_deg = d_user_pvt_solver->get_course_over_ground();
                    *UTC_time = convert_to_time_t(d_user_pvt_solver->get_position_UTC_time());

                    return true;
                }
        }
    else
        {
            if (d_internal_pvt_solver->is_valid_position())
                {
                    *latitude_deg = d_internal_pvt_solver->get_latitude();
                    *longitude_deg = d_internal_pvt_solver->get_longitude();
                    *height_m = d_internal_pvt_solver->get_height();
                    *ground_speed_kmh = d_internal_pvt_solver->get_speed_over_ground() * 3600.0 / 1000.0;
                    *course_over_ground_deg = d_internal_pvt_solver->get_course_over_ground();
                    *UTC_time = convert_to_time_t(d_internal_pvt_solver->get_position_UTC_time());

                    return true;
                }
        }

    return false;
}


void rtklib_pvt_gs::apply_rx_clock_offset(std::map<int, Gnss_Synchro>& observables_map,
    double rx_clock_offset_s)
{
    // apply corrections according to Rinex 3.04, Table 1: Observation Corrections for Receiver Clock Offset
    std::map<int, Gnss_Synchro>::iterator observables_iter;

    for (observables_iter = observables_map.begin(); observables_iter != observables_map.end(); observables_iter++)
        {
            // all observables in the map are valid
            observables_iter->second.RX_time -= rx_clock_offset_s;
            observables_iter->second.Pseudorange_m -= rx_clock_offset_s * SPEED_OF_LIGHT_M_S;

            switch (d_mapStringValues[observables_iter->second.Signal])
                {
                case evGPS_1C:
                case evSBAS_1C:
                case evGAL_1B:
                    observables_iter->second.Carrier_phase_rads -= rx_clock_offset_s * FREQ1 * TWO_PI;
                    break;
                case evGPS_L5:
                case evGAL_5X:
                    observables_iter->second.Carrier_phase_rads -= rx_clock_offset_s * FREQ5 * TWO_PI;
                    break;
                case evGPS_2S:
                    observables_iter->second.Carrier_phase_rads -= rx_clock_offset_s * FREQ2 * TWO_PI;
                    break;
                case evBDS_B3:
                    observables_iter->second.Carrier_phase_rads -= rx_clock_offset_s * FREQ3_BDS * TWO_PI;
                    break;
                case evGLO_1G:
                    observables_iter->second.Carrier_phase_rads -= rx_clock_offset_s * FREQ1_GLO * TWO_PI;
                    break;
                case evGLO_2G:
                    observables_iter->second.Carrier_phase_rads -= rx_clock_offset_s * FREQ2_GLO * TWO_PI;
                    break;
                case evBDS_B1:
                    observables_iter->second.Carrier_phase_rads -= rx_clock_offset_s * FREQ1_BDS * TWO_PI;
                    break;
                case evBDS_B2:
                    observables_iter->second.Carrier_phase_rads -= rx_clock_offset_s * FREQ2_BDS * TWO_PI;
                    break;
                default:
                    break;
                }
        }
}


std::map<int, Gnss_Synchro> rtklib_pvt_gs::interpolate_observables(const std::map<int, Gnss_Synchro>& observables_map_t0,
    const std::map<int, Gnss_Synchro>& observables_map_t1,
    double rx_time_s)
{
    std::map<int, Gnss_Synchro> interp_observables_map;
    // Linear interpolation: y(t) = y(t0) + (y(t1) - y(t0)) * (t - t0) / (t1 - t0)

    // check TOW rollover
    double time_factor;
    if ((observables_map_t1.cbegin()->second.RX_time -
            observables_map_t0.cbegin()->second.RX_time) > 0)
        {
            time_factor = (rx_time_s - observables_map_t0.cbegin()->second.RX_time) /
                          (observables_map_t1.cbegin()->second.RX_time -
                              observables_map_t0.cbegin()->second.RX_time);
        }
    else
        {
            // TOW rollover situation
            time_factor = (604800000.0 + rx_time_s - observables_map_t0.cbegin()->second.RX_time) /
                          (604800000.0 + observables_map_t1.cbegin()->second.RX_time -
                              observables_map_t0.cbegin()->second.RX_time);
        }

    std::map<int, Gnss_Synchro>::const_iterator observables_iter;
    for (observables_iter = observables_map_t0.cbegin(); observables_iter != observables_map_t0.cend(); observables_iter++)
        {
            // 1. Check if the observable exist in t0 and t1
            // the map key is the channel ID (see work())
            try
                {
                    if (observables_map_t1.at(observables_iter->first).PRN == observables_iter->second.PRN)
                        {
                            interp_observables_map.insert(std::pair<int, Gnss_Synchro>(observables_iter->first, observables_iter->second));
                            interp_observables_map.at(observables_iter->first).RX_time = rx_time_s;  // interpolation point
                            interp_observables_map.at(observables_iter->first).Pseudorange_m += (observables_map_t1.at(observables_iter->first).Pseudorange_m - observables_iter->second.Pseudorange_m) * time_factor;
                            interp_observables_map.at(observables_iter->first).Carrier_phase_rads += (observables_map_t1.at(observables_iter->first).Carrier_phase_rads - observables_iter->second.Carrier_phase_rads) * time_factor;
                            interp_observables_map.at(observables_iter->first).Carrier_Doppler_hz += (observables_map_t1.at(observables_iter->first).Carrier_Doppler_hz - observables_iter->second.Carrier_Doppler_hz) * time_factor;
                        }
                }
            catch (const std::out_of_range& oor)
                {
                    // observable does not exist in t1
                }
        }
    return interp_observables_map;
}


void rtklib_pvt_gs::initialize_and_apply_carrier_phase_offset()
{
    // we have a valid PVT. First check if we need to reset the initial carrier phase offsets to match their pseudoranges
    std::map<int, Gnss_Synchro>::iterator observables_iter;
    for (observables_iter = d_gnss_observables_map.begin(); observables_iter != d_gnss_observables_map.end(); observables_iter++)
        {
            // check if an initialization is required (new satellite or loss of lock)
            // it is set to false by the work function if the gnss_synchro is not valid
            if (d_channel_initialized.at(observables_iter->second.Channel_ID) == false)
                {
                    double wavelength_m = 0;
                    switch (d_mapStringValues[observables_iter->second.Signal])
                        {
                        case evGPS_1C:
                        case evSBAS_1C:
                        case evGAL_1B:
                            wavelength_m = SPEED_OF_LIGHT_M_S / FREQ1;
                            break;
                        case evGPS_L5:
                        case evGAL_5X:
                            wavelength_m = SPEED_OF_LIGHT_M_S / FREQ5;
                            break;
                        case evGPS_2S:
                            wavelength_m = SPEED_OF_LIGHT_M_S / FREQ2;
                            break;
                        case evBDS_B3:
                            wavelength_m = SPEED_OF_LIGHT_M_S / FREQ3_BDS;
                            break;
                        case evGLO_1G:
                            wavelength_m = SPEED_OF_LIGHT_M_S / FREQ1_GLO;
                            break;
                        case evGLO_2G:
                            wavelength_m = SPEED_OF_LIGHT_M_S / FREQ2_GLO;
                            break;
                        case evBDS_B1:
                            wavelength_m = SPEED_OF_LIGHT_M_S / FREQ1_BDS;
                            break;
                        case evBDS_B2:
                            wavelength_m = SPEED_OF_LIGHT_M_S / FREQ2_BDS;
                            break;
                        default:
                            break;
                        }
                    const double wrap_carrier_phase_rad = fmod(observables_iter->second.Carrier_phase_rads, TWO_PI);
                    d_initial_carrier_phase_offset_estimation_rads.at(observables_iter->second.Channel_ID) = TWO_PI * round(observables_iter->second.Pseudorange_m / wavelength_m) - observables_iter->second.Carrier_phase_rads + wrap_carrier_phase_rad;
                    d_channel_initialized.at(observables_iter->second.Channel_ID) = true;
                    DLOG(INFO) << "initialized carrier phase at channel " << observables_iter->second.Channel_ID;
                }
            // apply the carrier phase offset to this satellite
            observables_iter->second.Carrier_phase_rads = observables_iter->second.Carrier_phase_rads + d_initial_carrier_phase_offset_estimation_rads.at(observables_iter->second.Channel_ID);
        }
}


int rtklib_pvt_gs::work(int noutput_items, gr_vector_const_void_star& input_items,
    gr_vector_void_star& output_items __attribute__((unused)))
{
    for (int32_t epoch = 0; epoch < noutput_items; epoch++)
        {
            bool flag_display_pvt = false;
            bool flag_compute_pvt_output = false;
            bool flag_write_RTCM_1019_output = false;
            bool flag_write_RTCM_1020_output = false;
            bool flag_write_RTCM_1045_output = false;
            bool flag_write_RTCM_MSM_output = false;
            bool flag_write_RINEX_obs_output = false;

            d_gnss_observables_map.clear();
            const auto** in = reinterpret_cast<const Gnss_Synchro**>(&input_items[0]);  // Get the input buffer pointer
            // ############ 1. READ PSEUDORANGES ####
            for (uint32_t i = 0; i < d_nchannels; i++)
                {
                    if (in[i][epoch].Flag_valid_pseudorange)
                        {
                            const auto tmp_eph_iter_gps = d_internal_pvt_solver->gps_ephemeris_map.find(in[i][epoch].PRN);
                            const auto tmp_eph_iter_gal = d_internal_pvt_solver->galileo_ephemeris_map.find(in[i][epoch].PRN);
                            const auto tmp_eph_iter_cnav = d_internal_pvt_solver->gps_cnav_ephemeris_map.find(in[i][epoch].PRN);
                            const auto tmp_eph_iter_glo_gnav = d_internal_pvt_solver->glonass_gnav_ephemeris_map.find(in[i][epoch].PRN);
                            const auto tmp_eph_iter_bds_dnav = d_internal_pvt_solver->beidou_dnav_ephemeris_map.find(in[i][epoch].PRN);

                            bool store_valid_observable = false;

                            if (tmp_eph_iter_gps != d_internal_pvt_solver->gps_ephemeris_map.cend())
                                {
                                    const uint32_t prn_aux = tmp_eph_iter_gps->second.i_satellite_PRN;
                                    if ((prn_aux == in[i][epoch].PRN) and (std::string(in[i][epoch].Signal) == "1C"))
                                        {
                                            store_valid_observable = true;
                                        }
                                }
                            if (tmp_eph_iter_gal != d_internal_pvt_solver->galileo_ephemeris_map.cend())
                                {
                                    const uint32_t prn_aux = tmp_eph_iter_gal->second.i_satellite_PRN;
                                    if ((prn_aux == in[i][epoch].PRN) and ((std::string(in[i][epoch].Signal) == "1B") or (std::string(in[i][epoch].Signal) == "5X")))
                                        {
                                            store_valid_observable = true;
                                        }
                                }
                            if (tmp_eph_iter_cnav != d_internal_pvt_solver->gps_cnav_ephemeris_map.cend())
                                {
                                    const uint32_t prn_aux = tmp_eph_iter_cnav->second.i_satellite_PRN;
                                    if ((prn_aux == in[i][epoch].PRN) and ((std::string(in[i][epoch].Signal) == "2S") or (std::string(in[i][epoch].Signal) == "L5")))
                                        {
                                            store_valid_observable = true;
                                        }
                                }
                            if (tmp_eph_iter_glo_gnav != d_internal_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                {
                                    const uint32_t prn_aux = tmp_eph_iter_glo_gnav->second.i_satellite_PRN;
                                    if ((prn_aux == in[i][epoch].PRN) and ((std::string(in[i][epoch].Signal) == "1G") or (std::string(in[i][epoch].Signal) == "2G")))
                                        {
                                            store_valid_observable = true;
                                        }
                                }
                            if (tmp_eph_iter_bds_dnav != d_internal_pvt_solver->beidou_dnav_ephemeris_map.cend())
                                {
                                    const uint32_t prn_aux = tmp_eph_iter_bds_dnav->second.i_satellite_PRN;
                                    if ((prn_aux == in[i][epoch].PRN) and ((std::string(in[i][epoch].Signal) == "B1") or (std::string(in[i][epoch].Signal) == "B3")))
                                        {
                                            store_valid_observable = true;
                                        }
                                }

                            if (store_valid_observable)
                                {
                                    // store valid observables in a map.
                                    d_gnss_observables_map.insert(std::pair<int, Gnss_Synchro>(i, in[i][epoch]));
                                }

                            if (d_rtcm_enabled)
                                {
                                    try
                                        {
                                            if (d_internal_pvt_solver->gps_ephemeris_map.empty() == false)
                                                {
                                                    if (tmp_eph_iter_gps != d_internal_pvt_solver->gps_ephemeris_map.cend())
                                                        {
                                                            d_rtcm_printer->lock_time(d_internal_pvt_solver->gps_ephemeris_map.find(in[i][epoch].PRN)->second, in[i][epoch].RX_time, in[i][epoch]);  // keep track of locking time
                                                        }
                                                }
                                            if (d_internal_pvt_solver->galileo_ephemeris_map.empty() == false)
                                                {
                                                    if (tmp_eph_iter_gal != d_internal_pvt_solver->galileo_ephemeris_map.cend())
                                                        {
                                                            d_rtcm_printer->lock_time(d_internal_pvt_solver->galileo_ephemeris_map.find(in[i][epoch].PRN)->second, in[i][epoch].RX_time, in[i][epoch]);  // keep track of locking time
                                                        }
                                                }
                                            if (d_internal_pvt_solver->gps_cnav_ephemeris_map.empty() == false)
                                                {
                                                    if (tmp_eph_iter_cnav != d_internal_pvt_solver->gps_cnav_ephemeris_map.cend())
                                                        {
                                                            d_rtcm_printer->lock_time(d_internal_pvt_solver->gps_cnav_ephemeris_map.find(in[i][epoch].PRN)->second, in[i][epoch].RX_time, in[i][epoch]);  // keep track of locking time
                                                        }
                                                }
                                            if (d_internal_pvt_solver->glonass_gnav_ephemeris_map.empty() == false)
                                                {
                                                    if (tmp_eph_iter_glo_gnav != d_internal_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                        {
                                                            d_rtcm_printer->lock_time(d_internal_pvt_solver->glonass_gnav_ephemeris_map.find(in[i][epoch].PRN)->second, in[i][epoch].RX_time, in[i][epoch]);  // keep track of locking time
                                                        }
                                                }
                                        }
                                    catch (const boost::exception& ex)
                                        {
                                            std::cout << "RTCM boost exception: " << boost::diagnostic_information(ex) << '\n';
                                            LOG(ERROR) << "RTCM boost exception: " << boost::diagnostic_information(ex);
                                        }
                                    catch (const std::exception& ex)
                                        {
                                            std::cout << "RTCM std exception: " << ex.what() << '\n';
                                            LOG(ERROR) << "RTCM std exception: " << ex.what();
                                        }
                                }
                        }
                    else
                        {
                            d_channel_initialized.at(i) = false;  // the current channel is not reporting valid observable
                        }
                }

            // ############ 2 COMPUTE THE PVT ################################
            bool flag_pvt_valid = false;
            if (d_gnss_observables_map.empty() == false)
                {
                    // LOG(INFO) << "diff raw obs time: " << d_gnss_observables_map.cbegin()->second.RX_time * 1000.0 - old_time_debug;
                    // old_time_debug = d_gnss_observables_map.cbegin()->second.RX_time * 1000.0;
                    uint32_t current_RX_time_ms = 0;
                    // #### solve PVT and store the corrected observable set
                    if (d_internal_pvt_solver->get_PVT(d_gnss_observables_map, false))
                        {
                            const double Rx_clock_offset_s = d_internal_pvt_solver->get_time_offset_s();
                            if (fabs(Rx_clock_offset_s) * 1000.0 > d_max_obs_block_rx_clock_offset_ms)
                                {
                                    if (!d_waiting_obs_block_rx_clock_offset_correction_msg)
                                        {
                                            this->message_port_pub(pmt::mp("pvt_to_observables"), pmt::make_any(Rx_clock_offset_s));
                                            d_waiting_obs_block_rx_clock_offset_correction_msg = true;
                                            LOG(INFO) << "Sent clock offset correction to observables: " << Rx_clock_offset_s << "[s]";
                                        }
                                }
                            else
                                {
                                    if (d_enable_rx_clock_correction == true)
                                        {
                                            d_waiting_obs_block_rx_clock_offset_correction_msg = false;
                                            d_gnss_observables_map_t0 = d_gnss_observables_map_t1;
                                            apply_rx_clock_offset(d_gnss_observables_map, Rx_clock_offset_s);
                                            d_gnss_observables_map_t1 = d_gnss_observables_map;

                                            // ### select the rx_time and interpolate observables at that time
                                            if (!d_gnss_observables_map_t0.empty())
                                                {
                                                    const auto t0_int_ms = static_cast<uint32_t>(d_gnss_observables_map_t0.cbegin()->second.RX_time * 1000.0);
                                                    const uint32_t adjust_next_20ms = 20 - t0_int_ms % 20;
                                                    current_RX_time_ms = t0_int_ms + adjust_next_20ms;

                                                    if (current_RX_time_ms % d_output_rate_ms == 0)
                                                        {
                                                            d_rx_time = static_cast<double>(current_RX_time_ms) / 1000.0;
                                                            // std::cout << " obs time t0: " << d_gnss_observables_map_t0.cbegin()->second.RX_time
                                                            //           << " t1: " << d_gnss_observables_map_t1.cbegin()->second.RX_time
                                                            //           << " interp time: " << d_rx_time << '\n';
                                                            d_gnss_observables_map = interpolate_observables(d_gnss_observables_map_t0,
                                                                d_gnss_observables_map_t1,
                                                                d_rx_time);
                                                            flag_compute_pvt_output = true;
                                                            // d_rx_time = current_RX_time;
                                                            // std::cout.precision(17);
                                                            // std::cout << "current_RX_time: " << current_RX_time << " map time: " << d_gnss_observables_map.begin()->second.RX_time << '\n';
                                                        }
                                                }
                                        }
                                    else
                                        {
                                            d_rx_time = d_gnss_observables_map.begin()->second.RX_time;
                                            current_RX_time_ms = static_cast<uint32_t>(d_rx_time * 1000.0);
                                            if (current_RX_time_ms % d_output_rate_ms == 0)
                                                {
                                                    flag_compute_pvt_output = true;
                                                }
                                            flag_pvt_valid = true;
                                        }
                                }
                        }
                    // debug code
                    // else
                    //     {
                    //         DLOG(INFO) << "Internal PVT solver error";
                    //     }

                    // compute on the fly PVT solution
                    if (flag_compute_pvt_output == true)
                        {
                            flag_pvt_valid = d_user_pvt_solver->get_PVT(d_gnss_observables_map, false);
                        }

                    if (flag_pvt_valid == true)
                        {
                            // initialize (if needed) the accumulated phase offset and apply it to the active channels
                            // required to report accumulated phase cycles comparable to pseudoranges
                            initialize_and_apply_carrier_phase_offset();

                            const double Rx_clock_offset_s = d_user_pvt_solver->get_time_offset_s();
                            if (d_enable_rx_clock_correction == true and fabs(Rx_clock_offset_s) > 0.000001)  // 1us !!
                                {
                                    LOG(INFO) << "Warning: Rx clock offset at interpolated RX time: " << Rx_clock_offset_s * 1000.0 << "[ms]"
                                              << " at RX time: " << static_cast<uint32_t>(d_rx_time * 1000.0) << " [ms]";
                                }
                            else
                                {
                                    DLOG(INFO) << "Rx clock offset at interpolated RX time: " << Rx_clock_offset_s * 1000.0 << "[s]"
                                               << " at RX time: " << static_cast<uint32_t>(d_rx_time * 1000.0) << " [ms]";
                                    // Optional debug code: export observables snapshot for rtklib unit testing
                                    // std::cout << "step 1: save gnss_synchro map\n";
                                    // save_gnss_synchro_map_xml("./gnss_synchro_map.xml");
                                    // getchar(); // stop the execution
                                    // end debug
                                    if (d_display_rate_ms != 0)
                                        {
                                            if (current_RX_time_ms % d_display_rate_ms == 0)
                                                {
                                                    flag_display_pvt = true;
                                                }
                                        }
                                    if (d_rtcm_MT1019_rate_ms != 0)  // allows deactivating messages by setting rate = 0
                                        {
                                            if (current_RX_time_ms % d_rtcm_MT1019_rate_ms == 0)
                                                {
                                                    flag_write_RTCM_1019_output = true;
                                                }
                                        }
                                    if (d_rtcm_MT1020_rate_ms != 0)  // allows deactivating messages by setting rate = 0
                                        {
                                            if (current_RX_time_ms % d_rtcm_MT1020_rate_ms == 0)
                                                {
                                                    flag_write_RTCM_1020_output = true;
                                                }
                                        }
                                    if (d_rtcm_MT1045_rate_ms != 0)
                                        {
                                            if (current_RX_time_ms % d_rtcm_MT1045_rate_ms == 0)
                                                {
                                                    flag_write_RTCM_1045_output = true;
                                                }
                                        }
                                    // TODO: RTCM 1077, 1087 and 1097 are not used, so, disable the output rates
                                    // if (current_RX_time_ms % d_rtcm_MT1077_rate_ms==0 and d_rtcm_MT1077_rate_ms != 0)
                                    //     {
                                    //         last_RTCM_1077_output_time = current_RX_time;
                                    //     }
                                    // if (current_RX_time_ms % d_rtcm_MT1087_rate_ms==0 and d_rtcm_MT1087_rate_ms != 0)
                                    //     {
                                    //         last_RTCM_1087_output_time = current_RX_time;
                                    //     }
                                    // if (current_RX_time_ms % d_rtcm_MT1097_rate_ms==0 and d_rtcm_MT1097_rate_ms != 0)
                                    //     {
                                    //         last_RTCM_1097_output_time = current_RX_time;
                                    //     }
                                    if (d_rtcm_MSM_rate_ms != 0)
                                        {
                                            if (current_RX_time_ms % d_rtcm_MSM_rate_ms == 0)
                                                {
                                                    flag_write_RTCM_MSM_output = true;
                                                }
                                        }
                                    if (d_rinexobs_rate_ms != 0)
                                        {
                                            if (current_RX_time_ms % static_cast<uint32_t>(d_rinexobs_rate_ms) == 0)
                                                {
                                                    flag_write_RINEX_obs_output = true;
                                                }
                                        }

                                    if (d_first_fix == true)
                                        {
                                            if (d_show_local_time_zone)
                                                {
						    
                                                    const boost::posix_time::ptime time_first_solution = d_user_pvt_solver->get_position_UTC_time() + d_utc_diff_time;
                                                    std::cout << "First position fix at " << time_first_solution << d_local_time_str;
						    if(d_PPS_correction)
						    {
						        d_pps_init_offset=d_user_pvt_solver->get_time_offset_s();
						        d_pps_prev_error=0.;
						        d_estimator=0.;
						        LOG(INFO) << "init offset: " << d_pps_init_offset << "\n";
						        d_LO_external_frequ_init=d_LO_external_frequ;
						    }
                                                }
                                            else
                                                {
						    if(d_PPS_correction)
						    {
						        d_pps_init_offset=d_user_pvt_solver->get_time_offset_s();
						        d_pps_prev_error=0.;
						        d_estimator=0.;
						        LOG(INFO) << "init offset: " << d_pps_init_offset << "\n";
						        d_LO_external_frequ_init=d_LO_external_frequ;
						    }
                                                    std::cout << "First position fix at " << d_user_pvt_solver->get_position_UTC_time() << " UTC";
                                                }
                                            std::cout << " is Lat = " << d_user_pvt_solver->get_latitude() << " [deg], Long = " << d_user_pvt_solver->get_longitude()
                                                      << " [deg], Height= " << d_user_pvt_solver->get_height() << " [m]\n";
                                            d_ttff_msgbuf ttff;
                                            ttff.mtype = 1;
                                            d_end = std::chrono::system_clock::now();
                                            std::chrono::duration<double> elapsed_seconds = d_end - d_start;
                                            ttff.ttff = elapsed_seconds.count();
                                            send_sys_v_ttff_msg(ttff);
                                            d_first_fix = false;
                                        }
                                    if (d_kml_output_enabled)
                                        {
                                            if (current_RX_time_ms % d_kml_rate_ms == 0)
                                                {
                                                    d_kml_dump->print_position(d_user_pvt_solver.get(), false);
                                                }
                                        }
                                    if (d_gpx_output_enabled)
                                        {
                                            if (current_RX_time_ms % d_gpx_rate_ms == 0)
                                                {
                                                    d_gpx_dump->print_position(d_user_pvt_solver.get(), false);
                                                }
                                        }
                                    if (d_geojson_output_enabled)
                                        {
                                            if (current_RX_time_ms % d_geojson_rate_ms == 0)
                                                {
                                                    d_geojson_printer->print_position(d_user_pvt_solver.get(), false);
                                                }
                                        }
                                    if (d_nmea_output_file_enabled)
                                        {
                                            if (current_RX_time_ms % d_nmea_rate_ms == 0)
                                                {
                                                    d_nmea_printer->Print_Nmea_Line(d_user_pvt_solver.get(), false);
                                                }
                                        }

                                    /*
                                     *   TYPE  |  RECEIVER
                                     *     0   |  Unknown
                                     *     1   |  GPS L1 C/A
                                     *     2   |  GPS L2C
                                     *     3   |  GPS L5
                                     *     4   |  Galileo E1B
                                     *     5   |  Galileo E5a
                                     *     6   |  Galileo E5b
                                     *     7   |  GPS L1 C/A + GPS L2C
                                     *     8   |  GPS L1 C/A + GPS L5
                                     *     9   |  GPS L1 C/A + Galileo E1B
                                     *    10   |  GPS L1 C/A + Galileo E5a
                                     *    11   |  GPS L1 C/A + Galileo E5b
                                     *    12   |  Galileo E1B + GPS L2C
                                     *    13   |  Galileo E5a + GPS L5
                                     *    14   |  Galileo E1B + Galileo E5a
                                     *    15   |  Galileo E1B + Galileo E5b
                                     *    16   |  GPS L2C + GPS L5
                                     *    17   |  GPS L2C + Galileo E5a
                                     *    18   |  GPS L2C + Galileo E5b
                                     *    21   |  GPS L1 C/A + Galileo E1B + GPS L2C
                                     *    22   |  GPS L1 C/A + Galileo E1B + GPS L5
                                     *    23   |  GLONASS L1 C/A
                                     *    24   |  GLONASS L2 C/A
                                     *    25   |  GLONASS L1 C/A + GLONASS L2 C/A
                                     *    26   |  GPS L1 C/A + GLONASS L1 C/A
                                     *    27   |  Galileo E1B + GLONASS L1 C/A
                                     *    28   |  GPS L2C + GLONASS L1 C/A
                                     *    29   |  GPS L1 C/A + GLONASS L2 C/A
                                     *    30   |  Galileo E1B + GLONASS L2 C/A
                                     *    31   |  GPS L2C + GLONASS L2 C/A
                                     *    32   |  GPS L1 C/A + Galileo E1B + GPS L5 + Galileo E5a
                                     *    500   |  BeiDou B1I
                                     *    501   |  BeiDou B1I + GPS L1 C/A
                                     *    502   |  BeiDou B1I + Galileo E1B
                                     *    503   |  BeiDou B1I + GLONASS L1 C/A
                                     *    504   |  BeiDou B1I + GPS L1 C/A + Galileo E1B
                                     *    505   |  BeiDou B1I + GPS L1 C/A + GLONASS L1 C/A + Galileo E1B
                                     *    506   |  BeiDou B1I + Beidou B3I
                                     *    600   |  BeiDou B3I
                                     *    601   |  BeiDou B3I + GPS L2C
                                     *    602   |  BeiDou B3I + GLONASS L2 C/A
                                     *    603   |  BeiDou B3I + GPS L2C + GLONASS L2 C/A
                                     *    604   |  BeiDou B3I + GPS L1 C/A
                                     *    605   |  BeiDou B3I + Galileo E1B
                                     *    606   |  BeiDou B3I + GLONASS L1 C/A
                                     *    607   |  BeiDou B3I + GPS L1 C/A + Galileo E1B
                                     *    608   |  BeiDou B3I + GPS L1 C/A + Galileo E1B + BeiDou B1I
                                     *    609   |  BeiDou B3I + GPS L1 C/A + Galileo E1B + GLONASS L1 C/A
                                     *    610   |  BeiDou B3I + GPS L1 C/A + Galileo E1B + GLONASS L1 C/A + BeiDou B1I
                                     *   1000   |  GPS L1 C/A + GPS L2C + GPS L5
                                     *   1001   |  GPS L1 C/A + Galileo E1B + GPS L2C + GPS L5 + Galileo E5a
                                     */

                                    // ####################### RINEX FILES #################
                                    if (d_rinex_output_enabled)
                                        {
                                            std::map<int, Galileo_Ephemeris>::const_iterator galileo_ephemeris_iter;
                                            std::map<int, Gps_Ephemeris>::const_iterator gps_ephemeris_iter;
                                            std::map<int, Gps_CNAV_Ephemeris>::const_iterator gps_cnav_ephemeris_iter;
                                            std::map<int, Glonass_Gnav_Ephemeris>::const_iterator glonass_gnav_ephemeris_iter;
                                            std::map<int, Beidou_Dnav_Ephemeris>::const_iterator beidou_dnav_ephemeris_iter;
                                            if (!d_rinex_header_written)  // & we have utc data in nav message!
                                                {
                                                    galileo_ephemeris_iter = d_user_pvt_solver->galileo_ephemeris_map.cbegin();
                                                    gps_ephemeris_iter = d_user_pvt_solver->gps_ephemeris_map.cbegin();
                                                    gps_cnav_ephemeris_iter = d_user_pvt_solver->gps_cnav_ephemeris_map.cbegin();
                                                    glonass_gnav_ephemeris_iter = d_user_pvt_solver->glonass_gnav_ephemeris_map.cbegin();
                                                    beidou_dnav_ephemeris_iter = d_user_pvt_solver->beidou_dnav_ephemeris_map.cbegin();
                                                    switch (d_type_of_rx)
                                                        {
                                                        case 1:  // GPS L1 C/A only
                                                            if (gps_ephemeris_iter != d_user_pvt_solver->gps_ephemeris_map.cend())
                                                                {
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, gps_ephemeris_iter->second, d_rx_time);
                                                                    d_rp->rinex_nav_header(d_rp->navFile, d_user_pvt_solver->gps_iono, d_user_pvt_solver->gps_utc_model, gps_ephemeris_iter->second);
                                                                    d_rp->log_rinex_nav(d_rp->navFile, d_user_pvt_solver->gps_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 2:  // GPS L2C only
                                                            if (gps_cnav_ephemeris_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend())
                                                                {
                                                                    const std::string signal("2S");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, gps_cnav_ephemeris_iter->second, d_rx_time, signal);
                                                                    d_rp->rinex_nav_header(d_rp->navFile, d_user_pvt_solver->gps_cnav_iono, d_user_pvt_solver->gps_cnav_utc_model);
                                                                    d_rp->log_rinex_nav(d_rp->navFile, d_user_pvt_solver->gps_cnav_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 3:  // GPS L5 only
                                                            if (gps_cnav_ephemeris_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend())
                                                                {
                                                                    const std::string signal("L5");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, gps_cnav_ephemeris_iter->second, d_rx_time, signal);
                                                                    d_rp->rinex_nav_header(d_rp->navFile, d_user_pvt_solver->gps_cnav_iono, d_user_pvt_solver->gps_cnav_utc_model);
                                                                    d_rp->log_rinex_nav(d_rp->navFile, d_user_pvt_solver->gps_cnav_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 4:  // Galileo E1B only
                                                            if (galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                {
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, galileo_ephemeris_iter->second, d_rx_time);
                                                                    d_rp->rinex_nav_header(d_rp->navGalFile, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model);
                                                                    d_rp->log_rinex_nav(d_rp->navGalFile, d_user_pvt_solver->galileo_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 5:  // Galileo E5a only
                                                            if (galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                {
                                                                    const std::string signal("5X");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, galileo_ephemeris_iter->second, d_rx_time, signal);
                                                                    d_rp->rinex_nav_header(d_rp->navGalFile, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model);
                                                                    d_rp->log_rinex_nav(d_rp->navGalFile, d_user_pvt_solver->galileo_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 6:  // Galileo E5b only
                                                            if (galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                {
                                                                    const std::string signal("7X");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, galileo_ephemeris_iter->second, d_rx_time, signal);
                                                                    d_rp->rinex_nav_header(d_rp->navGalFile, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model);
                                                                    d_rp->log_rinex_nav(d_rp->navGalFile, d_user_pvt_solver->galileo_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 7:  // GPS L1 C/A + GPS L2C
                                                            if ((gps_ephemeris_iter != d_user_pvt_solver->gps_ephemeris_map.cend()) and (gps_cnav_ephemeris_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend()))
                                                                {
                                                                    const std::string signal("1C 2S");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, gps_ephemeris_iter->second, gps_cnav_ephemeris_iter->second, d_rx_time, signal);
                                                                    d_rp->rinex_nav_header(d_rp->navFile, d_user_pvt_solver->gps_iono, d_user_pvt_solver->gps_utc_model, gps_ephemeris_iter->second);
                                                                    d_rp->log_rinex_nav(d_rp->navFile, d_user_pvt_solver->gps_cnav_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 8:  // GPS L1 + GPS L5
                                                            if ((gps_ephemeris_iter != d_user_pvt_solver->gps_ephemeris_map.cend()) and (gps_cnav_ephemeris_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend()))
                                                                {
                                                                    const std::string signal("1C L5");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, gps_ephemeris_iter->second, gps_cnav_ephemeris_iter->second, d_rx_time, signal);
                                                                    d_rp->rinex_nav_header(d_rp->navFile, d_user_pvt_solver->gps_iono, d_user_pvt_solver->gps_utc_model, gps_ephemeris_iter->second);
                                                                    d_rp->log_rinex_nav(d_rp->navFile, d_user_pvt_solver->gps_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 9:  // GPS L1 C/A + Galileo E1B
                                                            if ((galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend()) and (gps_ephemeris_iter != d_user_pvt_solver->gps_ephemeris_map.cend()))
                                                                {
                                                                    const std::string gal_signal("1B");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, gps_ephemeris_iter->second, galileo_ephemeris_iter->second, d_rx_time, gal_signal);
                                                                    d_rp->rinex_nav_header(d_rp->navMixFile, d_user_pvt_solver->gps_iono, d_user_pvt_solver->gps_utc_model, gps_ephemeris_iter->second, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model);
                                                                    d_rp->log_rinex_nav(d_rp->navMixFile, d_user_pvt_solver->gps_ephemeris_map, d_user_pvt_solver->galileo_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 10:  // GPS L1 C/A + Galileo E5a
                                                            if ((galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend()) and (gps_ephemeris_iter != d_user_pvt_solver->gps_ephemeris_map.cend()))
                                                                {
                                                                    const std::string gal_signal("5X");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, gps_ephemeris_iter->second, galileo_ephemeris_iter->second, d_rx_time, gal_signal);
                                                                    d_rp->rinex_nav_header(d_rp->navMixFile, d_user_pvt_solver->gps_iono, d_user_pvt_solver->gps_utc_model, gps_ephemeris_iter->second, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model);
                                                                    d_rp->log_rinex_nav(d_rp->navMixFile, d_user_pvt_solver->gps_ephemeris_map, d_user_pvt_solver->galileo_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 11:  // GPS L1 C/A + Galileo E5b
                                                            if ((galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend()) and (gps_ephemeris_iter != d_user_pvt_solver->gps_ephemeris_map.cend()))
                                                                {
                                                                    const std::string gal_signal("7X");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, gps_ephemeris_iter->second, galileo_ephemeris_iter->second, d_rx_time, gal_signal);
                                                                    d_rp->rinex_nav_header(d_rp->navMixFile, d_user_pvt_solver->gps_iono, d_user_pvt_solver->gps_utc_model, gps_ephemeris_iter->second, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model);
                                                                    d_rp->log_rinex_nav(d_rp->navMixFile, d_user_pvt_solver->gps_ephemeris_map, d_user_pvt_solver->galileo_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 13:  // L5+E5a
                                                            if ((galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend()) and (gps_cnav_ephemeris_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend()))
                                                                {
                                                                    const std::string gal_signal("5X");
                                                                    const std::string gps_signal("L5");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, gps_cnav_ephemeris_iter->second, galileo_ephemeris_iter->second, d_rx_time, gps_signal, gal_signal);
                                                                    d_rp->rinex_nav_header(d_rp->navMixFile, d_user_pvt_solver->gps_cnav_iono, d_user_pvt_solver->gps_cnav_utc_model, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model);
                                                                    d_rp->log_rinex_nav(d_rp->navMixFile, d_user_pvt_solver->gps_cnav_ephemeris_map, d_user_pvt_solver->galileo_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 14:  // Galileo E1B + Galileo E5a
                                                            if ((galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend()))
                                                                {
                                                                    const std::string gal_signal("1B 5X");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, galileo_ephemeris_iter->second, d_rx_time, gal_signal);
                                                                    d_rp->rinex_nav_header(d_rp->navGalFile, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model);
                                                                    d_rp->log_rinex_nav(d_rp->navGalFile, d_user_pvt_solver->galileo_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 15:  // Galileo E1B + Galileo E5b
                                                            if ((galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend()))
                                                                {
                                                                    const std::string gal_signal("1B 7X");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, galileo_ephemeris_iter->second, d_rx_time, gal_signal);
                                                                    d_rp->rinex_nav_header(d_rp->navGalFile, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model);
                                                                    d_rp->log_rinex_nav(d_rp->navGalFile, d_user_pvt_solver->galileo_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 23:  // GLONASS L1 C/A only
                                                            if (glonass_gnav_ephemeris_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                {
                                                                    const std::string signal("1G");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, glonass_gnav_ephemeris_iter->second, d_rx_time, signal);
                                                                    d_rp->rinex_nav_header(d_rp->navGloFile, d_user_pvt_solver->glonass_gnav_utc_model, glonass_gnav_ephemeris_iter->second);
                                                                    d_rp->log_rinex_nav(d_rp->navGloFile, d_user_pvt_solver->glonass_gnav_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 24:  // GLONASS L2 C/A only
                                                            if (glonass_gnav_ephemeris_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                {
                                                                    const std::string signal("2G");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, glonass_gnav_ephemeris_iter->second, d_rx_time, signal);
                                                                    d_rp->rinex_nav_header(d_rp->navGloFile, d_user_pvt_solver->glonass_gnav_utc_model, glonass_gnav_ephemeris_iter->second);
                                                                    d_rp->log_rinex_nav(d_rp->navGloFile, d_user_pvt_solver->glonass_gnav_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 25:  // GLONASS L1 C/A + GLONASS L2 C/A
                                                            if (glonass_gnav_ephemeris_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                {
                                                                    const std::string signal("1G 2G");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, glonass_gnav_ephemeris_iter->second, d_rx_time, signal);
                                                                    d_rp->rinex_nav_header(d_rp->navGloFile, d_user_pvt_solver->glonass_gnav_utc_model, glonass_gnav_ephemeris_iter->second);
                                                                    d_rp->log_rinex_nav(d_rp->navGloFile, d_user_pvt_solver->glonass_gnav_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 26:  // GPS L1 C/A + GLONASS L1 C/A
                                                            if ((glonass_gnav_ephemeris_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend()) and (gps_ephemeris_iter != d_user_pvt_solver->gps_ephemeris_map.cend()))
                                                                {
                                                                    const std::string glo_signal("1G");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, gps_ephemeris_iter->second, glonass_gnav_ephemeris_iter->second, d_rx_time, glo_signal);
                                                                    if (d_rinex_version == 3)
                                                                        {
                                                                            d_rp->rinex_nav_header(d_rp->navMixFile, d_user_pvt_solver->gps_iono, d_user_pvt_solver->gps_utc_model, gps_ephemeris_iter->second, d_user_pvt_solver->glonass_gnav_utc_model, d_user_pvt_solver->glonass_gnav_almanac);
                                                                            d_rp->log_rinex_nav(d_rp->navMixFile, d_user_pvt_solver->gps_ephemeris_map, d_user_pvt_solver->glonass_gnav_ephemeris_map);
                                                                        }
                                                                    if (d_rinex_version == 2)
                                                                        {
                                                                            d_rp->rinex_nav_header(d_rp->navFile, d_user_pvt_solver->gps_iono, d_user_pvt_solver->gps_utc_model, gps_ephemeris_iter->second);
                                                                            d_rp->rinex_nav_header(d_rp->navGloFile, d_user_pvt_solver->glonass_gnav_utc_model, glonass_gnav_ephemeris_iter->second);
                                                                            d_rp->log_rinex_nav(d_rp->navFile, d_user_pvt_solver->gps_ephemeris_map);
                                                                            d_rp->log_rinex_nav(d_rp->navGloFile, d_user_pvt_solver->glonass_gnav_ephemeris_map);
                                                                        }
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 27:  // Galileo E1B + GLONASS L1 C/A
                                                            if ((glonass_gnav_ephemeris_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend()) and (galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend()))
                                                                {
                                                                    const std::string glo_signal("1G");
                                                                    const std::string gal_signal("1B");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, galileo_ephemeris_iter->second, glonass_gnav_ephemeris_iter->second, d_rx_time, glo_signal, gal_signal);
                                                                    d_rp->rinex_nav_header(d_rp->navMixFile, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model, d_user_pvt_solver->glonass_gnav_utc_model, d_user_pvt_solver->glonass_gnav_almanac);
                                                                    d_rp->log_rinex_nav(d_rp->navMixFile, d_user_pvt_solver->galileo_ephemeris_map, d_user_pvt_solver->glonass_gnav_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 28:  // GPS L2C + GLONASS L1 C/A
                                                            if ((glonass_gnav_ephemeris_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend()) and (gps_cnav_ephemeris_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend()))
                                                                {
                                                                    const std::string glo_signal("1G");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, gps_cnav_ephemeris_iter->second, glonass_gnav_ephemeris_iter->second, d_rx_time, glo_signal);
                                                                    d_rp->rinex_nav_header(d_rp->navMixFile, d_user_pvt_solver->gps_cnav_iono, d_user_pvt_solver->gps_cnav_utc_model, d_user_pvt_solver->glonass_gnav_utc_model, d_user_pvt_solver->glonass_gnav_almanac);
                                                                    d_rp->log_rinex_nav(d_rp->navMixFile, d_user_pvt_solver->gps_cnav_ephemeris_map, d_user_pvt_solver->glonass_gnav_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 29:  // GPS L1 C/A + GLONASS L2 C/A
                                                            if ((glonass_gnav_ephemeris_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend()) and (gps_ephemeris_iter != d_user_pvt_solver->gps_ephemeris_map.cend()))
                                                                {
                                                                    const std::string glo_signal("2G");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, gps_ephemeris_iter->second, glonass_gnav_ephemeris_iter->second, d_rx_time, glo_signal);
                                                                    if (d_rinex_version == 3)
                                                                        {
                                                                            d_rp->rinex_nav_header(d_rp->navMixFile, d_user_pvt_solver->gps_iono, d_user_pvt_solver->gps_utc_model, gps_ephemeris_iter->second, d_user_pvt_solver->glonass_gnav_utc_model, d_user_pvt_solver->glonass_gnav_almanac);
                                                                            d_rp->log_rinex_nav(d_rp->navMixFile, d_user_pvt_solver->gps_ephemeris_map, d_user_pvt_solver->glonass_gnav_ephemeris_map);
                                                                        }
                                                                    if (d_rinex_version == 2)
                                                                        {
                                                                            d_rp->rinex_nav_header(d_rp->navFile, d_user_pvt_solver->gps_iono, d_user_pvt_solver->gps_utc_model, gps_ephemeris_iter->second);
                                                                            d_rp->rinex_nav_header(d_rp->navGloFile, d_user_pvt_solver->glonass_gnav_utc_model, glonass_gnav_ephemeris_iter->second);
                                                                            d_rp->log_rinex_nav(d_rp->navFile, d_user_pvt_solver->gps_ephemeris_map);
                                                                            d_rp->log_rinex_nav(d_rp->navGloFile, d_user_pvt_solver->glonass_gnav_ephemeris_map);
                                                                        }
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 30:  // Galileo E1B + GLONASS L2 C/A
                                                            if ((glonass_gnav_ephemeris_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend()) and (galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend()))
                                                                {
                                                                    const std::string glo_signal("2G");
                                                                    const std::string gal_signal("1B");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, galileo_ephemeris_iter->second, glonass_gnav_ephemeris_iter->second, d_rx_time, glo_signal, gal_signal);
                                                                    d_rp->rinex_nav_header(d_rp->navMixFile, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model, d_user_pvt_solver->glonass_gnav_utc_model, d_user_pvt_solver->glonass_gnav_almanac);
                                                                    d_rp->log_rinex_nav(d_rp->navMixFile, d_user_pvt_solver->galileo_ephemeris_map, d_user_pvt_solver->glonass_gnav_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 31:  // GPS L2C + GLONASS L2 C/A
                                                            if ((glonass_gnav_ephemeris_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend()) and (gps_cnav_ephemeris_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend()))
                                                                {
                                                                    const std::string glo_signal("2G");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, gps_cnav_ephemeris_iter->second, glonass_gnav_ephemeris_iter->second, d_rx_time, glo_signal);
                                                                    d_rp->rinex_nav_header(d_rp->navMixFile, d_user_pvt_solver->gps_cnav_iono, d_user_pvt_solver->gps_cnav_utc_model, d_user_pvt_solver->glonass_gnav_utc_model, d_user_pvt_solver->glonass_gnav_almanac);
                                                                    d_rp->log_rinex_nav(d_rp->navMixFile, d_user_pvt_solver->gps_cnav_ephemeris_map, d_user_pvt_solver->glonass_gnav_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 32:  // L1+E1+L5+E5a
                                                            if ((gps_ephemeris_iter != d_user_pvt_solver->gps_ephemeris_map.cend()) and
                                                                (gps_cnav_ephemeris_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend()) and
                                                                (galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend()))
                                                                {
                                                                    const std::string gal_signal("1B 5X");
                                                                    const std::string gps_signal("1C L5");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, gps_ephemeris_iter->second, gps_cnav_ephemeris_iter->second, galileo_ephemeris_iter->second, d_rx_time, gps_signal, gal_signal);
                                                                    d_rp->rinex_nav_header(d_rp->navMixFile, d_user_pvt_solver->gps_iono, d_user_pvt_solver->gps_utc_model, gps_ephemeris_iter->second, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model);
                                                                    d_rp->log_rinex_nav(d_rp->navMixFile, d_user_pvt_solver->gps_ephemeris_map, d_user_pvt_solver->galileo_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 33:  // L1+E1+E5a
                                                            if ((gps_ephemeris_iter != d_user_pvt_solver->gps_ephemeris_map.cend()) and
                                                                (galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend()))
                                                                {
                                                                    const std::string gal_signal("1B 5X");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, gps_ephemeris_iter->second, galileo_ephemeris_iter->second, d_rx_time, gal_signal);
                                                                    d_rp->rinex_nav_header(d_rp->navMixFile, d_user_pvt_solver->gps_iono, d_user_pvt_solver->gps_utc_model, gps_ephemeris_iter->second, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model);
                                                                    d_rp->log_rinex_nav(d_rp->navMixFile, d_user_pvt_solver->gps_ephemeris_map, d_user_pvt_solver->galileo_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 500:  // BDS B1I only
                                                            if (beidou_dnav_ephemeris_iter != d_user_pvt_solver->beidou_dnav_ephemeris_map.cend())
                                                                {
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, beidou_dnav_ephemeris_iter->second, d_rx_time, "B1");
                                                                    d_rp->rinex_nav_header(d_rp->navFile, d_user_pvt_solver->beidou_dnav_iono, d_user_pvt_solver->beidou_dnav_utc_model);
                                                                    d_rp->log_rinex_nav(d_rp->navFile, d_user_pvt_solver->beidou_dnav_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }

                                                            break;
                                                        case 501:  // BeiDou B1I + GPS L1 C/A
                                                            if ((gps_ephemeris_iter != d_user_pvt_solver->gps_ephemeris_map.cend()) and (beidou_dnav_ephemeris_iter != d_user_pvt_solver->beidou_dnav_ephemeris_map.cend()))
                                                                {
                                                                    const std::string bds_signal("B1");
                                                                    // d_rp->rinex_obs_header(d_rp->obsFile, gps_ephemeris_iter->second, beidou_dnav_ephemeris_iter->second, d_rx_time, bds_signal);
                                                                    // d_rp->rinex_nav_header(d_rp->navMixFile, d_user_pvt_solver->gps_iono, d_user_pvt_solver->gps_utc_model, gps_ephemeris_iter->second, d_user_pvt_solver->beidou_dnav_iono, d_user_pvt_solver->beidou_dnav_utc_model);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }

                                                            break;
                                                        case 502:  // BeiDou B1I + Galileo E1B
                                                            if ((galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend()) and (beidou_dnav_ephemeris_iter != d_user_pvt_solver->beidou_dnav_ephemeris_map.cend()))
                                                                {
                                                                    const std::string bds_signal("B1");
                                                                    const std::string gal_signal("1B");
                                                                    // d_rp->rinex_obs_header(d_rp->obsFile, galileo_ephemeris_iter->second, beidou_dnav_ephemeris_iter->second, d_rx_time, gal_signal, bds_signal);
                                                                    // d_rp->rinex_nav_header(d_rp->navMixFile, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model, d_user_pvt_solver->beidou_dnav_iono, d_user_pvt_solver->beidou_dnav_utc_model);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }

                                                            break;
                                                        case 503:  // BeiDou B1I + GLONASS L1 C/A
                                                        case 504:  // BeiDou B1I + GPS L1 C/A + Galileo E1B
                                                        case 505:  // BeiDou B1I + GPS L1 C/A + GLONASS L1 C/A + Galileo E1B
                                                        case 506:  // BeiDou B1I + Beidou B3I
                                                            if (beidou_dnav_ephemeris_iter != d_user_pvt_solver->beidou_dnav_ephemeris_map.cend())
                                                                {
                                                                    // d_rp->rinex_obs_header(d_rp->obsFile, beidou_dnav_ephemeris_iter->second, d_rx_time, "B1");
                                                                    // d_rp->rinex_nav_header(d_rp->navFile, d_user_pvt_solver->beidou_dnav_iono, d_user_pvt_solver->beidou_dnav_utc_model);
                                                                    // d_rp->log_rinex_nav(d_rp->navFile, d_user_pvt_solver->beidou_dnav_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }

                                                            break;
                                                        case 600:  // BDS B3I only
                                                            if (beidou_dnav_ephemeris_iter != d_user_pvt_solver->beidou_dnav_ephemeris_map.cend())
                                                                {
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, beidou_dnav_ephemeris_iter->second, d_rx_time, "B3");
                                                                    d_rp->rinex_nav_header(d_rp->navFile, d_user_pvt_solver->beidou_dnav_iono, d_user_pvt_solver->beidou_dnav_utc_model);
                                                                    d_rp->log_rinex_nav(d_rp->navFile, d_user_pvt_solver->beidou_dnav_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }

                                                            break;
                                                        case 601:  // BeiDou B3I + GPS L2C
                                                        case 602:  // BeiDou B3I + GLONASS L2 C/A
                                                        case 603:  // BeiDou B3I + GPS L2C + GLONASS L2 C/A
                                                            if (beidou_dnav_ephemeris_iter != d_user_pvt_solver->beidou_dnav_ephemeris_map.cend())
                                                                {
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, beidou_dnav_ephemeris_iter->second, d_rx_time, "B3");
                                                                    // d_rp->rinex_nav_header(d_rp->navFile, d_user_pvt_solver->beidou_dnav_iono, d_user_pvt_solver->beidou_dnav_utc_model);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }

                                                            break;
                                                        case 1000:  // GPS L1 C/A + GPS L2C + GPS L5
                                                            if ((gps_ephemeris_iter != d_user_pvt_solver->gps_ephemeris_map.cend()) and
                                                                (gps_cnav_ephemeris_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend()))
                                                                {
                                                                    const std::string gps_signal("1C 2S L5");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, gps_ephemeris_iter->second, gps_cnav_ephemeris_iter->second, d_rx_time, gps_signal);
                                                                    d_rp->rinex_nav_header(d_rp->navFile, d_user_pvt_solver->gps_iono, d_user_pvt_solver->gps_utc_model, gps_ephemeris_iter->second);
                                                                    d_rp->log_rinex_nav(d_rp->navFile, d_user_pvt_solver->gps_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        case 1001:  // GPS L1 C/A + Galileo E1B + GPS L2C + GPS L5 + Galileo E5a
                                                            if ((galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend()) and
                                                                (gps_ephemeris_iter != d_user_pvt_solver->gps_ephemeris_map.cend()) and
                                                                (gps_cnav_ephemeris_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend()))
                                                                {
                                                                    const std::string gal_signal("1B 5X");
                                                                    const std::string gps_signal("1C 2S L5");
                                                                    d_rp->rinex_obs_header(d_rp->obsFile, gps_ephemeris_iter->second, gps_cnav_ephemeris_iter->second, galileo_ephemeris_iter->second, d_rx_time, gps_signal, gal_signal);
                                                                    d_rp->rinex_nav_header(d_rp->navMixFile, d_user_pvt_solver->gps_iono, d_user_pvt_solver->gps_utc_model, gps_ephemeris_iter->second, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model);
                                                                    d_rp->log_rinex_nav(d_rp->navMixFile, d_user_pvt_solver->gps_ephemeris_map, d_user_pvt_solver->galileo_ephemeris_map);
                                                                    d_rinex_header_written = true;  // do not write header anymore
                                                                }
                                                            break;
                                                        default:
                                                            break;
                                                        }
                                                }
                                            if (d_rinex_header_written)  // The header is already written, we can now log the navigation message data
                                                {
                                                    galileo_ephemeris_iter = d_user_pvt_solver->galileo_ephemeris_map.cbegin();
                                                    gps_ephemeris_iter = d_user_pvt_solver->gps_ephemeris_map.cbegin();
                                                    gps_cnav_ephemeris_iter = d_user_pvt_solver->gps_cnav_ephemeris_map.cbegin();
                                                    glonass_gnav_ephemeris_iter = d_user_pvt_solver->glonass_gnav_ephemeris_map.cbegin();
                                                    beidou_dnav_ephemeris_iter = d_user_pvt_solver->beidou_dnav_ephemeris_map.cbegin();

                                                    // Log observables into the RINEX file
                                                    if (flag_write_RINEX_obs_output)
                                                        {
                                                            switch (d_type_of_rx)
                                                                {
                                                                case 1:  // GPS L1 C/A only
                                                                    if (gps_ephemeris_iter != d_user_pvt_solver->gps_ephemeris_map.cend())
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, gps_ephemeris_iter->second, d_rx_time, d_gnss_observables_map);
                                                                            if (!d_rinex_header_updated and (d_user_pvt_solver->gps_utc_model.d_A0 != 0))
                                                                                {
                                                                                    d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->gps_utc_model);
                                                                                    d_rp->update_nav_header(d_rp->navFile, d_user_pvt_solver->gps_utc_model, d_user_pvt_solver->gps_iono, gps_ephemeris_iter->second);
                                                                                    d_rinex_header_updated = true;
                                                                                }
                                                                        }
                                                                    break;
                                                                case 2:  // GPS L2C only
                                                                case 3:  // GPS L5
                                                                    if (gps_cnav_ephemeris_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend())
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, gps_cnav_ephemeris_iter->second, d_rx_time, d_gnss_observables_map);
                                                                        }
                                                                    if (!d_rinex_header_updated and (d_user_pvt_solver->gps_cnav_utc_model.d_A0 != 0))
                                                                        {
                                                                            d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->gps_cnav_utc_model);
                                                                            d_rp->update_nav_header(d_rp->navFile, d_user_pvt_solver->gps_cnav_utc_model, d_user_pvt_solver->gps_cnav_iono);
                                                                            d_rinex_header_updated = true;
                                                                        }
                                                                    break;
                                                                case 4:  // Galileo E1B only
                                                                    if (galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, galileo_ephemeris_iter->second, d_rx_time, d_gnss_observables_map, "1B");
                                                                        }
                                                                    if (!d_rinex_header_updated and (d_user_pvt_solver->galileo_utc_model.A0_6 != 0))
                                                                        {
                                                                            d_rp->update_nav_header(d_rp->navGalFile, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model);
                                                                            d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->galileo_utc_model);
                                                                            d_rinex_header_updated = true;
                                                                        }
                                                                    break;
                                                                case 5:  // Galileo E5a only
                                                                    if (galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, galileo_ephemeris_iter->second, d_rx_time, d_gnss_observables_map, "5X");
                                                                        }
                                                                    if (!d_rinex_header_updated and (d_user_pvt_solver->galileo_utc_model.A0_6 != 0))
                                                                        {
                                                                            d_rp->update_nav_header(d_rp->navGalFile, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model);
                                                                            d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->galileo_utc_model);
                                                                            d_rinex_header_updated = true;
                                                                        }
                                                                    break;
                                                                case 6:  // Galileo E5b only
                                                                    if (galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, galileo_ephemeris_iter->second, d_rx_time, d_gnss_observables_map, "7X");
                                                                        }
                                                                    if (!d_rinex_header_updated and (d_user_pvt_solver->galileo_utc_model.A0_6 != 0))
                                                                        {
                                                                            d_rp->update_nav_header(d_rp->navGalFile, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model);
                                                                            d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->galileo_utc_model);
                                                                            d_rinex_header_updated = true;
                                                                        }
                                                                    break;
                                                                case 7:  // GPS L1 C/A + GPS L2C
                                                                    if ((gps_ephemeris_iter != d_user_pvt_solver->gps_ephemeris_map.cend()) and (gps_cnav_ephemeris_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend()))
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, gps_ephemeris_iter->second, gps_cnav_ephemeris_iter->second, d_rx_time, d_gnss_observables_map);
                                                                            if (!d_rinex_header_updated and (d_user_pvt_solver->gps_utc_model.d_A0 != 0))
                                                                                {
                                                                                    d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->gps_utc_model);
                                                                                    d_rp->update_nav_header(d_rp->navFile, d_user_pvt_solver->gps_utc_model, d_user_pvt_solver->gps_iono, gps_ephemeris_iter->second);
                                                                                    d_rinex_header_updated = true;
                                                                                }
                                                                        }
                                                                    break;
                                                                case 8:  // L1+L5
                                                                    if ((gps_ephemeris_iter != d_user_pvt_solver->gps_ephemeris_map.cend()) and (gps_cnav_ephemeris_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend()))
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, gps_ephemeris_iter->second, gps_cnav_ephemeris_iter->second, d_rx_time, d_gnss_observables_map);
                                                                            if (!d_rinex_header_updated and ((d_user_pvt_solver->gps_cnav_utc_model.d_A0 != 0) or (d_user_pvt_solver->gps_utc_model.d_A0 != 0)))
                                                                                {
                                                                                    if (d_user_pvt_solver->gps_cnav_utc_model.d_A0 != 0)
                                                                                        {
                                                                                            d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->gps_cnav_utc_model);
                                                                                            d_rp->update_nav_header(d_rp->navFile, d_user_pvt_solver->gps_cnav_utc_model, d_user_pvt_solver->gps_cnav_iono);
                                                                                        }
                                                                                    else
                                                                                        {
                                                                                            d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->gps_utc_model);
                                                                                            d_rp->update_nav_header(d_rp->navFile, d_user_pvt_solver->gps_utc_model, d_user_pvt_solver->gps_iono, gps_ephemeris_iter->second);
                                                                                        }
                                                                                    d_rinex_header_updated = true;
                                                                                }
                                                                        }
                                                                    break;
                                                                case 9:  // GPS L1 C/A + Galileo E1B
                                                                    if ((galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend()) and (gps_ephemeris_iter != d_user_pvt_solver->gps_ephemeris_map.cend()))
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, gps_ephemeris_iter->second, galileo_ephemeris_iter->second, d_rx_time, d_gnss_observables_map);
                                                                            if (!d_rinex_header_updated and (d_user_pvt_solver->gps_utc_model.d_A0 != 0))
                                                                                {
                                                                                    d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->gps_utc_model);
                                                                                    d_rp->update_nav_header(d_rp->navMixFile, d_user_pvt_solver->gps_iono, d_user_pvt_solver->gps_utc_model, gps_ephemeris_iter->second, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model);
                                                                                    d_rinex_header_updated = true;
                                                                                }
                                                                        }
                                                                    break;
                                                                case 13:  // L5+E5a
                                                                    if ((gps_cnav_ephemeris_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend()) and (galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend()))
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, gps_cnav_ephemeris_iter->second, galileo_ephemeris_iter->second, d_rx_time, d_gnss_observables_map);
                                                                        }
                                                                    if (!d_rinex_header_updated and (d_user_pvt_solver->gps_cnav_utc_model.d_A0 != 0) and (d_user_pvt_solver->galileo_utc_model.A0_6 != 0))
                                                                        {
                                                                            d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->gps_cnav_utc_model);
                                                                            d_rp->update_nav_header(d_rp->navMixFile, d_user_pvt_solver->gps_cnav_utc_model, d_user_pvt_solver->gps_cnav_iono, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model);
                                                                            d_rinex_header_updated = true;  // do not write header anymore
                                                                        }
                                                                    break;
                                                                case 14:  // Galileo E1B + Galileo E5a
                                                                    if (galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, galileo_ephemeris_iter->second, d_rx_time, d_gnss_observables_map, "1B 5X");
                                                                        }
                                                                    if (!d_rinex_header_updated and (d_user_pvt_solver->galileo_utc_model.A0_6 != 0))
                                                                        {
                                                                            d_rp->update_nav_header(d_rp->navGalFile, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model);
                                                                            d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->galileo_utc_model);
                                                                            d_rinex_header_updated = true;
                                                                        }
                                                                    break;
                                                                case 15:  // Galileo E1B + Galileo E5b
                                                                    if (galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, galileo_ephemeris_iter->second, d_rx_time, d_gnss_observables_map, "1B 7X");
                                                                        }
                                                                    if (!d_rinex_header_updated and (d_user_pvt_solver->galileo_utc_model.A0_6 != 0))
                                                                        {
                                                                            d_rp->update_nav_header(d_rp->navGalFile, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model);
                                                                            d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->galileo_utc_model);
                                                                            d_rinex_header_updated = true;
                                                                        }
                                                                    break;
                                                                case 23:  // GLONASS L1 C/A only
                                                                    if (glonass_gnav_ephemeris_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, glonass_gnav_ephemeris_iter->second, d_rx_time, d_gnss_observables_map, "1C");
                                                                        }
                                                                    if (!d_rinex_header_updated and (d_user_pvt_solver->glonass_gnav_utc_model.d_tau_c != 0))
                                                                        {
                                                                            d_rp->update_nav_header(d_rp->navGloFile, d_user_pvt_solver->glonass_gnav_utc_model, d_user_pvt_solver->glonass_gnav_almanac);
                                                                            d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->glonass_gnav_utc_model);
                                                                            d_rinex_header_updated = true;
                                                                        }
                                                                    break;
                                                                case 24:  // GLONASS L2 C/A only
                                                                    if (glonass_gnav_ephemeris_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, glonass_gnav_ephemeris_iter->second, d_rx_time, d_gnss_observables_map, "2C");
                                                                        }
                                                                    if (!d_rinex_header_updated and (d_user_pvt_solver->glonass_gnav_utc_model.d_tau_c != 0))
                                                                        {
                                                                            d_rp->update_nav_header(d_rp->navGloFile, d_user_pvt_solver->glonass_gnav_utc_model, d_user_pvt_solver->glonass_gnav_almanac);
                                                                            d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->glonass_gnav_utc_model);
                                                                            d_rinex_header_updated = true;
                                                                        }
                                                                    break;
                                                                case 25:  // GLONASS L1 C/A + GLONASS L2 C/A
                                                                    if (glonass_gnav_ephemeris_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, glonass_gnav_ephemeris_iter->second, d_rx_time, d_gnss_observables_map, "1C 2C");
                                                                        }
                                                                    if (!d_rinex_header_updated and (d_user_pvt_solver->glonass_gnav_utc_model.d_tau_c != 0))
                                                                        {
                                                                            d_rp->update_nav_header(d_rp->navMixFile, d_user_pvt_solver->glonass_gnav_utc_model, d_user_pvt_solver->glonass_gnav_almanac);
                                                                            d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->glonass_gnav_utc_model);
                                                                            d_rinex_header_updated = true;
                                                                        }
                                                                    break;
                                                                case 26:  // GPS L1 C/A + GLONASS L1 C/A
                                                                    if ((glonass_gnav_ephemeris_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend()) and (gps_ephemeris_iter != d_user_pvt_solver->gps_ephemeris_map.cend()))
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, gps_ephemeris_iter->second, glonass_gnav_ephemeris_iter->second, d_rx_time, d_gnss_observables_map);
                                                                            if (!d_rinex_header_updated and (d_user_pvt_solver->gps_utc_model.d_A0 != 0))
                                                                                {
                                                                                    d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->gps_utc_model);
                                                                                    d_rp->update_nav_header(d_rp->navMixFile, d_user_pvt_solver->gps_iono, d_user_pvt_solver->gps_utc_model, gps_ephemeris_iter->second, d_user_pvt_solver->glonass_gnav_utc_model, d_user_pvt_solver->glonass_gnav_almanac);
                                                                                    d_rinex_header_updated = true;  // do not write header anymore
                                                                                }
                                                                        }
                                                                    break;
                                                                case 27:  // Galileo E1B + GLONASS L1 C/A
                                                                    if ((glonass_gnav_ephemeris_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend()) and (galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend()))
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, galileo_ephemeris_iter->second, glonass_gnav_ephemeris_iter->second, d_rx_time, d_gnss_observables_map);
                                                                        }
                                                                    if (!d_rinex_header_updated and (d_user_pvt_solver->galileo_utc_model.A0_6 != 0))
                                                                        {
                                                                            d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->galileo_utc_model);
                                                                            d_rp->update_nav_header(d_rp->navMixFile, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model, d_user_pvt_solver->glonass_gnav_utc_model, d_user_pvt_solver->glonass_gnav_almanac);
                                                                            d_rinex_header_updated = true;  // do not write header anymore
                                                                        }
                                                                    break;
                                                                case 28:  // GPS L2C + GLONASS L1 C/A
                                                                    if ((glonass_gnav_ephemeris_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend()) and (gps_cnav_ephemeris_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend()))
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, gps_cnav_ephemeris_iter->second, glonass_gnav_ephemeris_iter->second, d_rx_time, d_gnss_observables_map);
                                                                        }
                                                                    if (!d_rinex_header_updated and (d_user_pvt_solver->gps_cnav_utc_model.d_A0 != 0))
                                                                        {
                                                                            d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->gps_cnav_utc_model);
                                                                            d_rp->update_nav_header(d_rp->navMixFile, d_user_pvt_solver->gps_cnav_iono, d_user_pvt_solver->gps_cnav_utc_model, d_user_pvt_solver->glonass_gnav_utc_model, d_user_pvt_solver->glonass_gnav_almanac);
                                                                            d_rinex_header_updated = true;  // do not write header anymore
                                                                        }
                                                                    break;
                                                                case 29:  // GPS L1 C/A + GLONASS L2 C/A
                                                                    if ((glonass_gnav_ephemeris_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend()) and (gps_ephemeris_iter != d_user_pvt_solver->gps_ephemeris_map.cend()))
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, gps_ephemeris_iter->second, glonass_gnav_ephemeris_iter->second, d_rx_time, d_gnss_observables_map);
                                                                            if (!d_rinex_header_updated and (d_user_pvt_solver->gps_utc_model.d_A0 != 0))
                                                                                {
                                                                                    d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->gps_utc_model);
                                                                                    d_rp->update_nav_header(d_rp->navMixFile, d_user_pvt_solver->gps_iono, d_user_pvt_solver->gps_utc_model, gps_ephemeris_iter->second, d_user_pvt_solver->glonass_gnav_utc_model, d_user_pvt_solver->glonass_gnav_almanac);
                                                                                    d_rinex_header_updated = true;  // do not write header anymore
                                                                                }
                                                                        }
                                                                    break;
                                                                case 30:  // Galileo E1B + GLONASS L2 C/A
                                                                    if ((glonass_gnav_ephemeris_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend()) and (galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend()))
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, galileo_ephemeris_iter->second, glonass_gnav_ephemeris_iter->second, d_rx_time, d_gnss_observables_map);
                                                                        }
                                                                    if (!d_rinex_header_updated and (d_user_pvt_solver->galileo_utc_model.A0_6 != 0))
                                                                        {
                                                                            d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->galileo_utc_model);
                                                                            d_rp->update_nav_header(d_rp->navMixFile, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model, d_user_pvt_solver->glonass_gnav_utc_model, d_user_pvt_solver->glonass_gnav_almanac);
                                                                            d_rinex_header_updated = true;  // do not write header anymore
                                                                        }
                                                                    break;
                                                                case 31:  // GPS L2C + GLONASS L2 C/A
                                                                    if ((glonass_gnav_ephemeris_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend()) and (gps_cnav_ephemeris_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend()))
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, gps_cnav_ephemeris_iter->second, glonass_gnav_ephemeris_iter->second, d_rx_time, d_gnss_observables_map);
                                                                        }
                                                                    if (!d_rinex_header_updated and (d_user_pvt_solver->gps_cnav_utc_model.d_A0 != 0))
                                                                        {
                                                                            d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->gps_cnav_utc_model);
                                                                            d_rp->update_nav_header(d_rp->navMixFile, d_user_pvt_solver->gps_cnav_iono, d_user_pvt_solver->gps_cnav_utc_model, d_user_pvt_solver->glonass_gnav_utc_model, d_user_pvt_solver->glonass_gnav_almanac);
                                                                            d_rinex_header_updated = true;  // do not write header anymore
                                                                        }
                                                                    break;
                                                                case 32:  // L1+E1+L5+E5a
                                                                    if ((gps_ephemeris_iter != d_user_pvt_solver->gps_ephemeris_map.cend()) and (gps_cnav_ephemeris_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend()) and (galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend()))
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, gps_ephemeris_iter->second, gps_cnav_ephemeris_iter->second, galileo_ephemeris_iter->second, d_rx_time, d_gnss_observables_map);
                                                                            if (!d_rinex_header_updated and ((d_user_pvt_solver->gps_cnav_utc_model.d_A0 != 0) or (d_user_pvt_solver->gps_utc_model.d_A0 != 0)) and (d_user_pvt_solver->galileo_utc_model.A0_6 != 0))
                                                                                {
                                                                                    if (d_user_pvt_solver->gps_cnav_utc_model.d_A0 != 0)
                                                                                        {
                                                                                            d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->gps_cnav_utc_model);
                                                                                            d_rp->update_nav_header(d_rp->navMixFile, d_user_pvt_solver->gps_cnav_utc_model, d_user_pvt_solver->gps_cnav_iono, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model);
                                                                                        }
                                                                                    else
                                                                                        {
                                                                                            d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->gps_utc_model);
                                                                                            d_rp->update_nav_header(d_rp->navMixFile, d_user_pvt_solver->gps_iono, d_user_pvt_solver->gps_utc_model, gps_ephemeris_iter->second, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model);
                                                                                        }
                                                                                    d_rinex_header_updated = true;  // do not write header anymore
                                                                                }
                                                                        }
                                                                    break;
                                                                case 33:  // L1+E1+E5a
                                                                    if ((gps_ephemeris_iter != d_user_pvt_solver->gps_ephemeris_map.cend()) and (galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend()))
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, gps_ephemeris_iter->second, galileo_ephemeris_iter->second, d_rx_time, d_gnss_observables_map);
                                                                            if (!d_rinex_header_updated and (d_user_pvt_solver->gps_utc_model.d_A0 != 0) and (d_user_pvt_solver->galileo_utc_model.A0_6 != 0))
                                                                                {
                                                                                    d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->gps_utc_model);
                                                                                    d_rp->update_nav_header(d_rp->navMixFile, d_user_pvt_solver->gps_iono, d_user_pvt_solver->gps_utc_model, gps_ephemeris_iter->second, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model);
                                                                                    d_rinex_header_updated = true;  // do not write header anymore
                                                                                }
                                                                        }
                                                                    break;
                                                                case 500:  // BDS B1I only
                                                                    if (beidou_dnav_ephemeris_iter != d_user_pvt_solver->beidou_dnav_ephemeris_map.cend())
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, beidou_dnav_ephemeris_iter->second, d_rx_time, d_gnss_observables_map, "B1");
                                                                        }
                                                                    if (!d_rinex_header_updated and (d_user_pvt_solver->beidou_dnav_utc_model.d_A0_UTC != 0))
                                                                        {
                                                                            d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->beidou_dnav_utc_model);
                                                                            d_rp->update_nav_header(d_rp->navFile, d_user_pvt_solver->beidou_dnav_utc_model, d_user_pvt_solver->beidou_dnav_iono);
                                                                            d_rinex_header_updated = true;
                                                                        }
                                                                    break;
                                                                case 600:  // BDS B3I only
                                                                    if (beidou_dnav_ephemeris_iter != d_user_pvt_solver->beidou_dnav_ephemeris_map.cend())
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, beidou_dnav_ephemeris_iter->second, d_rx_time, d_gnss_observables_map, "B3");
                                                                        }
                                                                    if (!d_rinex_header_updated and (d_user_pvt_solver->beidou_dnav_utc_model.d_A0_UTC != 0))
                                                                        {
                                                                            d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->beidou_dnav_utc_model);
                                                                            d_rp->update_nav_header(d_rp->navFile, d_user_pvt_solver->beidou_dnav_utc_model, d_user_pvt_solver->beidou_dnav_iono);
                                                                            d_rinex_header_updated = true;
                                                                        }
                                                                    break;
                                                                case 1000:  // GPS L1 C/A + GPS L2C + GPS L5
                                                                    if ((gps_ephemeris_iter != d_user_pvt_solver->gps_ephemeris_map.cend()) and
                                                                        (gps_cnav_ephemeris_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend()))
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, gps_ephemeris_iter->second, gps_cnav_ephemeris_iter->second, d_rx_time, d_gnss_observables_map, true);
                                                                        }
                                                                    if (!d_rinex_header_updated and (d_user_pvt_solver->gps_utc_model.d_A0 != 0))
                                                                        {
                                                                            d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->gps_utc_model);
                                                                            d_rp->update_nav_header(d_rp->navFile, d_user_pvt_solver->gps_utc_model, d_user_pvt_solver->gps_iono, gps_ephemeris_iter->second);
                                                                            d_rinex_header_updated = true;
                                                                        }
                                                                    break;
                                                                case 1001:  // GPS L1 C/A + Galileo E1B + GPS L2C + GPS L5 + Galileo E5a
                                                                    if ((galileo_ephemeris_iter != d_user_pvt_solver->galileo_ephemeris_map.cend()) and
                                                                        (gps_ephemeris_iter != d_user_pvt_solver->gps_ephemeris_map.cend()) and
                                                                        (gps_cnav_ephemeris_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend()))
                                                                        {
                                                                            d_rp->log_rinex_obs(d_rp->obsFile, gps_ephemeris_iter->second, gps_cnav_ephemeris_iter->second, galileo_ephemeris_iter->second, d_rx_time, d_gnss_observables_map, true);
                                                                        }
                                                                    if (!d_rinex_header_updated and (d_user_pvt_solver->gps_utc_model.d_A0 != 0) and (d_user_pvt_solver->galileo_utc_model.A0_6 != 0))
                                                                        {
                                                                            d_rp->update_obs_header(d_rp->obsFile, d_user_pvt_solver->gps_utc_model);
                                                                            d_rp->update_nav_header(d_rp->navMixFile, d_user_pvt_solver->gps_iono, d_user_pvt_solver->gps_utc_model, gps_ephemeris_iter->second, d_user_pvt_solver->galileo_iono, d_user_pvt_solver->galileo_utc_model);
                                                                            d_rinex_header_updated = true;
                                                                        }
                                                                    break;
                                                                default:
                                                                    break;
                                                                }
                                                        }
                                                }
                                        }

                                    // ####################### RTCM MESSAGES #################
                                    try
                                        {
                                            if (d_rtcm_writing_started and d_rtcm_enabled)
                                                {
                                                    switch (d_type_of_rx)
                                                        {
                                                        case 1:  // GPS L1 C/A
                                                            if (flag_write_RTCM_1019_output == true)
                                                                {
                                                                    for (const auto& gps_eph_iter : d_user_pvt_solver->gps_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1019(gps_eph_iter.second);
                                                                        }
                                                                }
                                                            if (flag_write_RTCM_MSM_output == true)
                                                                {
                                                                    const auto gps_eph_iter = d_user_pvt_solver->gps_ephemeris_map.cbegin();
                                                                    if (gps_eph_iter != d_user_pvt_solver->gps_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, gps_eph_iter->second, {}, {}, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            break;
                                                        case 4:
                                                        case 5:
                                                        case 6:
                                                            if (flag_write_RTCM_1045_output == true)
                                                                {
                                                                    for (const auto& gal_eph_iter : d_user_pvt_solver->galileo_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1045(gal_eph_iter.second);
                                                                        }
                                                                }
                                                            if (flag_write_RTCM_MSM_output == true)
                                                                {
                                                                    const auto gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.cbegin();
                                                                    if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, gal_eph_iter->second, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            break;
                                                        case 7:  // GPS L1 C/A + GPS L2C
                                                            if (flag_write_RTCM_1019_output == true)
                                                                {
                                                                    for (const auto& gps_eph_iter : d_user_pvt_solver->gps_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1019(gps_eph_iter.second);
                                                                        }
                                                                }
                                                            if (flag_write_RTCM_MSM_output == true)
                                                                {
                                                                    const auto gps_eph_iter = d_user_pvt_solver->gps_ephemeris_map.cbegin();
                                                                    const auto gps_cnav_eph_iter = d_user_pvt_solver->gps_cnav_ephemeris_map.cbegin();
                                                                    if ((gps_eph_iter != d_user_pvt_solver->gps_ephemeris_map.cend()) and (gps_cnav_eph_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend()))
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, gps_eph_iter->second, gps_cnav_eph_iter->second, {}, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            break;
                                                        case 8:  // L1+L5
                                                            if (flag_write_RTCM_1019_output == true)
                                                                {
                                                                    for (const auto& gps_eph_iter : d_user_pvt_solver->gps_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1019(gps_eph_iter.second);
                                                                        }
                                                                }
                                                            if (flag_write_RTCM_MSM_output == true)
                                                                {
                                                                    const auto gps_eph_iter = d_user_pvt_solver->gps_ephemeris_map.cbegin();
                                                                    const auto gps_cnav_eph_iter = d_user_pvt_solver->gps_cnav_ephemeris_map.cbegin();
                                                                    if ((gps_eph_iter != d_user_pvt_solver->gps_ephemeris_map.cend()) and (gps_cnav_eph_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend()))
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, gps_eph_iter->second, gps_cnav_eph_iter->second, {}, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            break;
                                                        case 9:  // GPS L1 C/A + Galileo E1B
                                                            if (flag_write_RTCM_1019_output == true)
                                                                {
                                                                    for (const auto& gps_eph_iter : d_user_pvt_solver->gps_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1019(gps_eph_iter.second);
                                                                        }
                                                                }
                                                            if (flag_write_RTCM_1045_output == true)
                                                                {
                                                                    for (const auto& gal_eph_iter : d_user_pvt_solver->galileo_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1045(gal_eph_iter.second);
                                                                        }
                                                                }
                                                            if (flag_write_RTCM_MSM_output == true)
                                                                {
                                                                    auto gps_eph_iter = d_user_pvt_solver->gps_ephemeris_map.cbegin();
                                                                    auto gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.cbegin();
                                                                    int gps_channel = 0;
                                                                    int gal_channel = 0;
                                                                    for (const auto& gnss_observables_iter : d_gnss_observables_map)
                                                                        {
                                                                            const std::string system(gnss_observables_iter.second.System, 1);
                                                                            if (gps_channel == 0)
                                                                                {
                                                                                    if (system == "G")
                                                                                        {
                                                                                            // This is a channel with valid GPS signal
                                                                                            gps_eph_iter = d_user_pvt_solver->gps_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (gps_eph_iter != d_user_pvt_solver->gps_ephemeris_map.cend())
                                                                                                {
                                                                                                    gps_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                            if (gal_channel == 0)
                                                                                {
                                                                                    if (system == "E")
                                                                                        {
                                                                                            gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                                                {
                                                                                                    gal_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                        }
                                                                    if (gps_eph_iter != d_user_pvt_solver->gps_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, gps_eph_iter->second, {}, {}, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                    if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, gal_eph_iter->second, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            break;
                                                        case 13:  // L5+E5a
                                                            if (flag_write_RTCM_1045_output == true)
                                                                {
                                                                    for (const auto& gal_eph_iter : d_user_pvt_solver->galileo_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1045(gal_eph_iter.second);
                                                                        }
                                                                }

                                                            if (flag_write_RTCM_MSM_output and d_rtcm_MSM_rate_ms != 0)
                                                                {
                                                                    auto gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.cbegin();
                                                                    auto gps_cnav_eph_iter = d_user_pvt_solver->gps_cnav_ephemeris_map.cbegin();
                                                                    int gal_channel = 0;
                                                                    int gps_channel = 0;
                                                                    for (const auto& gnss_observables_iter : d_gnss_observables_map)
                                                                        {
                                                                            const std::string system(gnss_observables_iter.second.System, 1);
                                                                            if (gps_channel == 0)
                                                                                {
                                                                                    if (system == "G")
                                                                                        {
                                                                                            // This is a channel with valid GPS signal
                                                                                            gps_cnav_eph_iter = d_user_pvt_solver->gps_cnav_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (gps_cnav_eph_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend())
                                                                                                {
                                                                                                    gps_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                            if (gal_channel == 0)
                                                                                {
                                                                                    if (system == "E")
                                                                                        {
                                                                                            gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                                                {
                                                                                                    gal_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                        }

                                                                    if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend() and (d_rtcm_MT1097_rate_ms != 0))
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, gal_eph_iter->second, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                    if (gps_cnav_eph_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend() and (d_rtcm_MT1077_rate_ms != 0))
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, gps_cnav_eph_iter->second, {}, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            break;
                                                        case 14:
                                                        case 15:
                                                            if (flag_write_RTCM_1045_output == true)
                                                                {
                                                                    for (const auto& gal_eph_iter : d_user_pvt_solver->galileo_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1045(gal_eph_iter.second);
                                                                        }
                                                                }
                                                            if (flag_write_RTCM_MSM_output == true)
                                                                {
                                                                    const auto gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.cbegin();
                                                                    if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, gal_eph_iter->second, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            break;
                                                        case 23:
                                                        case 24:
                                                        case 25:
                                                            if (flag_write_RTCM_1020_output == true)
                                                                {
                                                                    for (const auto& glonass_gnav_ephemeris_iter : d_user_pvt_solver->glonass_gnav_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1020(glonass_gnav_ephemeris_iter.second, d_user_pvt_solver->glonass_gnav_utc_model);
                                                                        }
                                                                }
                                                            if (flag_write_RTCM_MSM_output == true)
                                                                {
                                                                    const auto glo_gnav_ephemeris_iter = d_user_pvt_solver->glonass_gnav_ephemeris_map.cbegin();
                                                                    if (glo_gnav_ephemeris_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, {}, glo_gnav_ephemeris_iter->second, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            d_rtcm_writing_started = true;
                                                            break;
                                                        case 26:  // GPS L1 C/A + GLONASS L1 C/A
                                                            if (flag_write_RTCM_1019_output == true)
                                                                {
                                                                    for (const auto& gps_eph_iter : d_user_pvt_solver->gps_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1019(gps_eph_iter.second);
                                                                        }
                                                                }
                                                            if (flag_write_RTCM_1020_output == true)
                                                                {
                                                                    for (const auto& glonass_gnav_ephemeris_iter : d_user_pvt_solver->glonass_gnav_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1020(glonass_gnav_ephemeris_iter.second, d_user_pvt_solver->glonass_gnav_utc_model);
                                                                        }
                                                                }
                                                            if (flag_write_RTCM_MSM_output == true)
                                                                {
                                                                    auto glonass_gnav_eph_iter = d_user_pvt_solver->glonass_gnav_ephemeris_map.cbegin();
                                                                    auto gps_eph_iter = d_user_pvt_solver->gps_ephemeris_map.cbegin();
                                                                    int gps_channel = 0;
                                                                    int glo_channel = 0;
                                                                    for (const auto& gnss_observables_iter : d_gnss_observables_map)
                                                                        {
                                                                            const std::string system(gnss_observables_iter.second.System, 1);
                                                                            if (gps_channel == 0)
                                                                                {
                                                                                    if (system == "G")
                                                                                        {
                                                                                            // This is a channel with valid GPS signal
                                                                                            gps_eph_iter = d_user_pvt_solver->gps_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (gps_eph_iter != d_user_pvt_solver->gps_ephemeris_map.cend())
                                                                                                {
                                                                                                    gps_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                            if (glo_channel == 0)
                                                                                {
                                                                                    if (system == "R")
                                                                                        {
                                                                                            glonass_gnav_eph_iter = d_user_pvt_solver->glonass_gnav_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (glonass_gnav_eph_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                                                {
                                                                                                    glo_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                        }

                                                                    if (glonass_gnav_eph_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, {}, glonass_gnav_eph_iter->second, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                    if (gps_eph_iter != d_user_pvt_solver->gps_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, gps_eph_iter->second, {}, {}, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            break;
                                                        case 27:  // GLONASS L1 C/A + Galileo E1B
                                                            if (flag_write_RTCM_1020_output == true)
                                                                {
                                                                    for (const auto& glonass_gnav_eph_iter : d_user_pvt_solver->glonass_gnav_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1020(glonass_gnav_eph_iter.second, d_user_pvt_solver->glonass_gnav_utc_model);
                                                                        }
                                                                }
                                                            if (flag_write_RTCM_1045_output == true)
                                                                {
                                                                    for (const auto& gal_eph_iter : d_user_pvt_solver->galileo_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1045(gal_eph_iter.second);
                                                                        }
                                                                }
                                                            if (flag_write_RTCM_MSM_output == true)
                                                                {
                                                                    auto gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.cbegin();
                                                                    auto glonass_gnav_eph_iter = d_user_pvt_solver->glonass_gnav_ephemeris_map.cbegin();
                                                                    int gal_channel = 0;
                                                                    int glo_channel = 0;
                                                                    for (const auto& gnss_observables_iter : d_gnss_observables_map)
                                                                        {
                                                                            const std::string system(gnss_observables_iter.second.System, 1);
                                                                            if (gal_channel == 0)
                                                                                {
                                                                                    if (system == "E")
                                                                                        {
                                                                                            // This is a channel with valid GPS signal
                                                                                            gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                                                {
                                                                                                    gal_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                            if (glo_channel == 0)
                                                                                {
                                                                                    if (system == "R")
                                                                                        {
                                                                                            glonass_gnav_eph_iter = d_user_pvt_solver->glonass_gnav_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (glonass_gnav_eph_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                                                {
                                                                                                    glo_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                        }
                                                                    if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, gal_eph_iter->second, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                    if (glonass_gnav_eph_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, {}, glonass_gnav_eph_iter->second, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            break;
                                                        case 29:  // GPS L1 C/A + GLONASS L2 C/A
                                                            if (flag_write_RTCM_1019_output == true)
                                                                {
                                                                    for (const auto& gps_eph_iter : d_user_pvt_solver->gps_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1019(gps_eph_iter.second);
                                                                        }
                                                                }
                                                            if (flag_write_RTCM_1020_output == true)
                                                                {
                                                                    for (const auto& glonass_gnav_eph_iter : d_user_pvt_solver->glonass_gnav_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1020(glonass_gnav_eph_iter.second, d_user_pvt_solver->glonass_gnav_utc_model);
                                                                        }
                                                                }
                                                            if (flag_write_RTCM_MSM_output == true)
                                                                {
                                                                    auto gps_eph_iter = d_user_pvt_solver->gps_ephemeris_map.cbegin();
                                                                    auto glonass_gnav_eph_iter = d_user_pvt_solver->glonass_gnav_ephemeris_map.cbegin();
                                                                    int gps_channel = 0;
                                                                    int glo_channel = 0;
                                                                    for (const auto& gnss_observables_iter : d_gnss_observables_map)
                                                                        {
                                                                            const std::string system(gnss_observables_iter.second.System, 1);
                                                                            if (gps_channel == 0)
                                                                                {
                                                                                    if (system == "G")
                                                                                        {
                                                                                            // This is a channel with valid GPS signal
                                                                                            gps_eph_iter = d_user_pvt_solver->gps_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (gps_eph_iter != d_user_pvt_solver->gps_ephemeris_map.cend())
                                                                                                {
                                                                                                    gps_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                            if (glo_channel == 0)
                                                                                {
                                                                                    if (system == "R")
                                                                                        {
                                                                                            glonass_gnav_eph_iter = d_user_pvt_solver->glonass_gnav_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (glonass_gnav_eph_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                                                {
                                                                                                    glo_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                        }
                                                                    if (glonass_gnav_eph_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, {}, glonass_gnav_eph_iter->second, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                    if (gps_eph_iter != d_user_pvt_solver->gps_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, gps_eph_iter->second, {}, {}, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            break;
                                                        case 30:  // GLONASS L2 C/A + Galileo E1B
                                                            if (flag_write_RTCM_1020_output == true)
                                                                {
                                                                    for (const auto& glonass_gnav_eph_iter : d_user_pvt_solver->glonass_gnav_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1020(glonass_gnav_eph_iter.second, d_user_pvt_solver->glonass_gnav_utc_model);
                                                                        }
                                                                }
                                                            if (flag_write_RTCM_1045_output == true)
                                                                {
                                                                    for (const auto& gal_eph_iter : d_user_pvt_solver->galileo_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1045(gal_eph_iter.second);
                                                                        }
                                                                }
                                                            if (flag_write_RTCM_MSM_output == true)
                                                                {
                                                                    auto gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.cbegin();
                                                                    auto glonass_gnav_eph_iter = d_user_pvt_solver->glonass_gnav_ephemeris_map.cbegin();
                                                                    int gal_channel = 0;
                                                                    int glo_channel = 0;
                                                                    for (const auto& gnss_observables_iter : d_gnss_observables_map)
                                                                        {
                                                                            const std::string system(gnss_observables_iter.second.System, 1);
                                                                            if (gal_channel == 0)
                                                                                {
                                                                                    if (system == "E")
                                                                                        {
                                                                                            // This is a channel with valid GPS signal
                                                                                            gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                                                {
                                                                                                    gal_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                            if (glo_channel == 0)
                                                                                {
                                                                                    if (system == "R")
                                                                                        {
                                                                                            glonass_gnav_eph_iter = d_user_pvt_solver->glonass_gnav_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (glonass_gnav_eph_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                                                {
                                                                                                    glo_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                        }
                                                                    if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, gal_eph_iter->second, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                    if (glonass_gnav_eph_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, {}, glonass_gnav_eph_iter->second, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            break;
                                                        case 32:  // L1+E1+L5+E5a
                                                            if (flag_write_RTCM_1019_output == true)
                                                                {
                                                                    for (const auto& gps_eph_iter : d_user_pvt_solver->gps_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1019(gps_eph_iter.second);
                                                                        }
                                                                }
                                                            if (flag_write_RTCM_1045_output == true)
                                                                {
                                                                    for (const auto& gal_eph_iter : d_user_pvt_solver->galileo_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1045(gal_eph_iter.second);
                                                                        }
                                                                }
                                                            if (flag_write_RTCM_MSM_output == true)
                                                                {
                                                                    auto gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.cbegin();
                                                                    auto gps_eph_iter = d_user_pvt_solver->gps_ephemeris_map.cbegin();
                                                                    int gal_channel = 0;
                                                                    int gps_channel = 0;
                                                                    for (const auto& gnss_observables_iter : d_gnss_observables_map)
                                                                        {
                                                                            const std::string system(gnss_observables_iter.second.System, 1);
                                                                            if (gal_channel == 0)
                                                                                {
                                                                                    if (system == "E")
                                                                                        {
                                                                                            // This is a channel with valid GPS signal
                                                                                            gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                                                {
                                                                                                    gal_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                            if (gps_channel == 0)
                                                                                {
                                                                                    if (system == "G")
                                                                                        {
                                                                                            gps_eph_iter = d_user_pvt_solver->gps_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (gps_eph_iter != d_user_pvt_solver->gps_ephemeris_map.cend())
                                                                                                {
                                                                                                    gps_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                        }
                                                                    if (gps_eph_iter != d_user_pvt_solver->gps_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, gps_eph_iter->second, {}, {}, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                    if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, gal_eph_iter->second, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            break;
                                                        default:
                                                            break;
                                                        }
                                                }

                                            if (!d_rtcm_writing_started and d_rtcm_enabled)  // the first time
                                                {
                                                    switch (d_type_of_rx)
                                                        {
                                                        case 1:                              // GPS L1 C/A
                                                            if (d_rtcm_MT1019_rate_ms != 0)  // allows deactivating messages by setting rate = 0
                                                                {
                                                                    for (const auto& gps_eph_iter : d_user_pvt_solver->gps_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1019(gps_eph_iter.second);
                                                                        }
                                                                }
                                                            if (d_rtcm_MSM_rate_ms != 0)  // allows deactivating messages by setting rate = 0
                                                                {
                                                                    const auto gps_eph_iter = d_user_pvt_solver->gps_ephemeris_map.cbegin();
                                                                    if (gps_eph_iter != d_user_pvt_solver->gps_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, gps_eph_iter->second, {}, {}, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            d_rtcm_writing_started = true;
                                                            break;
                                                        case 4:
                                                        case 5:
                                                        case 6:
                                                            if (d_rtcm_MT1045_rate_ms != 0)  // allows deactivating messages by setting rate = 0
                                                                {
                                                                    for (const auto& gal_eph_iter : d_user_pvt_solver->galileo_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1045(gal_eph_iter.second);
                                                                        }
                                                                }
                                                            if (d_rtcm_MSM_rate_ms != 0)
                                                                {
                                                                    const auto gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.cbegin();
                                                                    if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, gal_eph_iter->second, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            d_rtcm_writing_started = true;
                                                            break;
                                                        case 7:                              // GPS L1 C/A + GPS L2C
                                                            if (d_rtcm_MT1019_rate_ms != 0)  // allows deactivating messages by setting rate = 0
                                                                {
                                                                    for (const auto& gps_eph_iter : d_user_pvt_solver->gps_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1019(gps_eph_iter.second);
                                                                        }
                                                                }
                                                            if (d_rtcm_MSM_rate_ms != 0)
                                                                {
                                                                    const auto gps_eph_iter = d_user_pvt_solver->gps_ephemeris_map.cbegin();
                                                                    const auto gps_cnav_eph_iter = d_user_pvt_solver->gps_cnav_ephemeris_map.cbegin();
                                                                    if ((gps_eph_iter != d_user_pvt_solver->gps_ephemeris_map.cend()) and (gps_cnav_eph_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend()))
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, gps_eph_iter->second, gps_cnav_eph_iter->second, {}, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            d_rtcm_writing_started = true;
                                                            break;
                                                        case 8:                              // L1+L5
                                                            if (d_rtcm_MT1019_rate_ms != 0)  // allows deactivating messages by setting rate = 0
                                                                {
                                                                    for (const auto& gps_eph_iter : d_user_pvt_solver->gps_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1019(gps_eph_iter.second);
                                                                        }
                                                                }
                                                            if (d_rtcm_MSM_rate_ms != 0)
                                                                {
                                                                    const auto gps_eph_iter = d_user_pvt_solver->gps_ephemeris_map.cbegin();
                                                                    const auto gps_cnav_eph_iter = d_user_pvt_solver->gps_cnav_ephemeris_map.cbegin();
                                                                    if ((gps_eph_iter != d_user_pvt_solver->gps_ephemeris_map.cend()) and (gps_cnav_eph_iter != d_user_pvt_solver->gps_cnav_ephemeris_map.cend()))
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, gps_eph_iter->second, gps_cnav_eph_iter->second, {}, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            d_rtcm_writing_started = true;
                                                            break;
                                                        case 9:                              // GPS L1 C/A + Galileo E1B
                                                            if (d_rtcm_MT1019_rate_ms != 0)  // allows deactivating messages by setting rate = 0
                                                                {
                                                                    for (const auto& gps_eph_iter : d_user_pvt_solver->gps_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1019(gps_eph_iter.second);
                                                                        }
                                                                }
                                                            if (d_rtcm_MT1045_rate_ms != 0)
                                                                {
                                                                    for (const auto& gal_eph_iter : d_user_pvt_solver->galileo_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1045(gal_eph_iter.second);
                                                                        }
                                                                }
                                                            if (d_rtcm_MSM_rate_ms != 0)
                                                                {
                                                                    auto gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.cbegin();
                                                                    auto gps_eph_iter = d_user_pvt_solver->gps_ephemeris_map.cbegin();
                                                                    int gps_channel = 0;
                                                                    int gal_channel = 0;
                                                                    for (const auto& gnss_observables_iter : d_gnss_observables_map)
                                                                        {
                                                                            const std::string system(gnss_observables_iter.second.System, 1);
                                                                            if (gps_channel == 0)
                                                                                {
                                                                                    if (system == "G")
                                                                                        {
                                                                                            // This is a channel with valid GPS signal
                                                                                            gps_eph_iter = d_user_pvt_solver->gps_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (gps_eph_iter != d_user_pvt_solver->gps_ephemeris_map.cend())
                                                                                                {
                                                                                                    gps_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                            if (gal_channel == 0)
                                                                                {
                                                                                    if (system == "E")
                                                                                        {
                                                                                            gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                                                {
                                                                                                    gal_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                        }
                                                                    if (gps_eph_iter != d_user_pvt_solver->gps_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, gps_eph_iter->second, {}, {}, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                    if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, gal_eph_iter->second, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            d_rtcm_writing_started = true;
                                                            break;

                                                        case 13:  // L5+E5a
                                                            if (d_rtcm_MT1045_rate_ms != 0)
                                                                {
                                                                    for (const auto& gal_eph_iter : d_user_pvt_solver->galileo_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1045(gal_eph_iter.second);
                                                                        }
                                                                }
                                                            if (d_rtcm_MSM_rate_ms != 0)
                                                                {
                                                                    auto gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.cbegin();
                                                                    int gal_channel = 0;
                                                                    for (const auto& gnss_observables_iter : d_gnss_observables_map)
                                                                        {
                                                                            const std::string system(gnss_observables_iter.second.System, 1);
                                                                            if (gal_channel == 0)
                                                                                {
                                                                                    if (system == "E")
                                                                                        {
                                                                                            gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                                                {
                                                                                                    gal_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                        }

                                                                    if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend() and (d_rtcm_MT1097_rate_ms != 0))
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, gal_eph_iter->second, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            d_rtcm_writing_started = true;
                                                            break;
                                                        case 14:
                                                        case 15:
                                                            if (d_rtcm_MT1045_rate_ms != 0)  // allows deactivating messages by setting rate = 0
                                                                {
                                                                    for (const auto& gal_eph_iter : d_user_pvt_solver->galileo_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1045(gal_eph_iter.second);
                                                                        }
                                                                }
                                                            if (d_rtcm_MSM_rate_ms != 0)
                                                                {
                                                                    const auto gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.cbegin();
                                                                    if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, gal_eph_iter->second, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            d_rtcm_writing_started = true;
                                                            break;
                                                        case 23:
                                                        case 24:
                                                        case 25:
                                                            if (d_rtcm_MT1020_rate_ms != 0)  // allows deactivating messages by setting rate = 0
                                                                {
                                                                    for (const auto& glonass_gnav_eph_iter : d_user_pvt_solver->glonass_gnav_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1020(glonass_gnav_eph_iter.second, d_user_pvt_solver->glonass_gnav_utc_model);
                                                                        }
                                                                }
                                                            if (d_rtcm_MSM_rate_ms != 0)
                                                                {
                                                                    const auto glo_gnav_ephemeris_iter = d_user_pvt_solver->glonass_gnav_ephemeris_map.cbegin();
                                                                    if (glo_gnav_ephemeris_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, {}, glo_gnav_ephemeris_iter->second, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            d_rtcm_writing_started = true;
                                                            break;
                                                        case 26:                             // GPS L1 C/A + GLONASS L1 C/A
                                                            if (d_rtcm_MT1019_rate_ms != 0)  // allows deactivating messages by setting rate = 0
                                                                {
                                                                    for (const auto& gps_eph_iter : d_user_pvt_solver->gps_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1019(gps_eph_iter.second);
                                                                        }
                                                                }
                                                            if (d_rtcm_MT1020_rate_ms != 0)  // allows deactivating messages by setting rate = 0
                                                                {
                                                                    for (const auto& glonass_gnav_eph_iter : d_user_pvt_solver->glonass_gnav_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1020(glonass_gnav_eph_iter.second, d_user_pvt_solver->glonass_gnav_utc_model);
                                                                        }
                                                                }
                                                            if (d_rtcm_MSM_rate_ms != 0)
                                                                {
                                                                    auto gps_eph_iter = d_user_pvt_solver->gps_ephemeris_map.cbegin();
                                                                    auto glonass_gnav_eph_iter = d_user_pvt_solver->glonass_gnav_ephemeris_map.cbegin();
                                                                    int gps_channel = 0;
                                                                    int glo_channel = 0;
                                                                    for (const auto& gnss_observables_iter : d_gnss_observables_map)
                                                                        {
                                                                            const std::string system(gnss_observables_iter.second.System, 1);
                                                                            if (gps_channel == 0)
                                                                                {
                                                                                    if (system == "G")
                                                                                        {
                                                                                            // This is a channel with valid GPS signal
                                                                                            gps_eph_iter = d_user_pvt_solver->gps_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (gps_eph_iter != d_user_pvt_solver->gps_ephemeris_map.cend())
                                                                                                {
                                                                                                    gps_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                            if (glo_channel == 0)
                                                                                {
                                                                                    if (system == "R")
                                                                                        {
                                                                                            glonass_gnav_eph_iter = d_user_pvt_solver->glonass_gnav_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (glonass_gnav_eph_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                                                {
                                                                                                    glo_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                        }
                                                                    if (glonass_gnav_eph_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, {}, glonass_gnav_eph_iter->second, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                    if (gps_eph_iter != d_user_pvt_solver->gps_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, gps_eph_iter->second, {}, {}, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            d_rtcm_writing_started = true;
                                                            break;
                                                        case 27:                             // GLONASS L1 C/A + Galileo E1B
                                                            if (d_rtcm_MT1020_rate_ms != 0)  // allows deactivating messages by setting rate = 0
                                                                {
                                                                    for (const auto& glonass_gnav_eph_iter : d_user_pvt_solver->glonass_gnav_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1020(glonass_gnav_eph_iter.second, d_user_pvt_solver->glonass_gnav_utc_model);
                                                                        }
                                                                }
                                                            if (d_rtcm_MT1045_rate_ms != 0)  // allows deactivating messages by setting rate = 0
                                                                {
                                                                    for (const auto& gal_eph_iter : d_user_pvt_solver->galileo_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1045(gal_eph_iter.second);
                                                                        }
                                                                }
                                                            if (d_rtcm_MSM_rate_ms != 0)
                                                                {
                                                                    int gal_channel = 0;
                                                                    int glo_channel = 0;
                                                                    auto gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.cbegin();
                                                                    auto glonass_gnav_eph_iter = d_user_pvt_solver->glonass_gnav_ephemeris_map.cbegin();
                                                                    for (const auto& gnss_observables_iter : d_gnss_observables_map)
                                                                        {
                                                                            const std::string system(gnss_observables_iter.second.System, 1);
                                                                            if (gal_channel == 0)
                                                                                {
                                                                                    if (system == "E")
                                                                                        {
                                                                                            // This is a channel with valid GPS signal
                                                                                            gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                                                {
                                                                                                    gal_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                            if (glo_channel == 0)
                                                                                {
                                                                                    if (system == "R")
                                                                                        {
                                                                                            glonass_gnav_eph_iter = d_user_pvt_solver->glonass_gnav_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (glonass_gnav_eph_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                                                {
                                                                                                    glo_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                        }
                                                                    if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, gal_eph_iter->second, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                    if (glonass_gnav_eph_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, {}, glonass_gnav_eph_iter->second, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            d_rtcm_writing_started = true;
                                                            break;
                                                        case 29:                             // GPS L1 C/A + GLONASS L2 C/A
                                                            if (d_rtcm_MT1019_rate_ms != 0)  // allows deactivating messages by setting rate = 0
                                                                {
                                                                    for (const auto& gps_eph_iter : d_user_pvt_solver->gps_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1019(gps_eph_iter.second);
                                                                        }
                                                                }
                                                            if (d_rtcm_MT1020_rate_ms != 0)  // allows deactivating messages by setting rate = 0
                                                                {
                                                                    for (const auto& glonass_gnav_eph_iter : d_user_pvt_solver->glonass_gnav_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1020(glonass_gnav_eph_iter.second, d_user_pvt_solver->glonass_gnav_utc_model);
                                                                        }
                                                                }
                                                            if (d_rtcm_MSM_rate_ms != 0)
                                                                {
                                                                    auto gps_eph_iter = d_user_pvt_solver->gps_ephemeris_map.cbegin();
                                                                    auto glonass_gnav_eph_iter = d_user_pvt_solver->glonass_gnav_ephemeris_map.cbegin();
                                                                    int gps_channel = 0;
                                                                    int glo_channel = 0;
                                                                    for (const auto& gnss_observables_iter : d_gnss_observables_map)
                                                                        {
                                                                            const std::string system(gnss_observables_iter.second.System, 1);
                                                                            if (gps_channel == 0)
                                                                                {
                                                                                    if (system == "G")
                                                                                        {
                                                                                            // This is a channel with valid GPS signal
                                                                                            gps_eph_iter = d_user_pvt_solver->gps_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (gps_eph_iter != d_user_pvt_solver->gps_ephemeris_map.cend())
                                                                                                {
                                                                                                    gps_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                            if (glo_channel == 0)
                                                                                {
                                                                                    if (system == "R")
                                                                                        {
                                                                                            glonass_gnav_eph_iter = d_user_pvt_solver->glonass_gnav_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (glonass_gnav_eph_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                                                {
                                                                                                    glo_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                        }
                                                                    if (glonass_gnav_eph_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, {}, glonass_gnav_eph_iter->second, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }

                                                                    if (gps_eph_iter != d_user_pvt_solver->gps_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, gps_eph_iter->second, {}, {}, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            d_rtcm_writing_started = true;
                                                            break;
                                                        case 30:                             // GLONASS L2 C/A + Galileo E1B
                                                            if (d_rtcm_MT1020_rate_ms != 0)  // allows deactivating messages by setting rate = 0
                                                                {
                                                                    for (const auto& glonass_gnav_eph_iter : d_user_pvt_solver->glonass_gnav_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1020(glonass_gnav_eph_iter.second, d_user_pvt_solver->glonass_gnav_utc_model);
                                                                        }
                                                                }
                                                            if (d_rtcm_MT1045_rate_ms != 0)  // allows deactivating messages by setting rate = 0
                                                                {
                                                                    for (const auto& gal_eph_iter : d_user_pvt_solver->galileo_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1045(gal_eph_iter.second);
                                                                        }
                                                                }
                                                            if (d_rtcm_MSM_rate_ms != 0)
                                                                {
                                                                    int gal_channel = 0;
                                                                    int glo_channel = 0;
                                                                    auto gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.cbegin();
                                                                    auto glonass_gnav_eph_iter = d_user_pvt_solver->glonass_gnav_ephemeris_map.cbegin();
                                                                    for (const auto& gnss_observables_iter : d_gnss_observables_map)
                                                                        {
                                                                            const std::string system(gnss_observables_iter.second.System, 1);
                                                                            if (gal_channel == 0)
                                                                                {
                                                                                    if (system == "E")
                                                                                        {
                                                                                            // This is a channel with valid GPS signal
                                                                                            gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                                                {
                                                                                                    gal_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                            if (glo_channel == 0)
                                                                                {
                                                                                    if (system == "R")
                                                                                        {
                                                                                            glonass_gnav_eph_iter = d_user_pvt_solver->glonass_gnav_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (glonass_gnav_eph_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                                                {
                                                                                                    glo_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                        }
                                                                    if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, gal_eph_iter->second, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                    if (glonass_gnav_eph_iter != d_user_pvt_solver->glonass_gnav_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, {}, glonass_gnav_eph_iter->second, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            d_rtcm_writing_started = true;
                                                            break;
                                                        case 32:                             // L1+E1+L5+E5a
                                                            if (d_rtcm_MT1019_rate_ms != 0)  // allows deactivating messages by setting rate = 0
                                                                {
                                                                    for (const auto& gps_eph_iter : d_user_pvt_solver->gps_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1019(gps_eph_iter.second);
                                                                        }
                                                                }
                                                            if (d_rtcm_MT1045_rate_ms != 0)  // allows deactivating messages by setting rate = 0
                                                                {
                                                                    for (const auto& gal_eph_iter : d_user_pvt_solver->galileo_ephemeris_map)
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MT1045(gal_eph_iter.second);
                                                                        }
                                                                }
                                                            if (d_rtcm_MSM_rate_ms != 0)
                                                                {
                                                                    auto gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.cbegin();
                                                                    auto gps_eph_iter = d_user_pvt_solver->gps_ephemeris_map.cbegin();
                                                                    int gps_channel = 0;
                                                                    int gal_channel = 0;
                                                                    for (const auto& gnss_observables_iter : d_gnss_observables_map)
                                                                        {
                                                                            const std::string system(gnss_observables_iter.second.System, 1);
                                                                            if (gps_channel == 0)
                                                                                {
                                                                                    if (system == "G")
                                                                                        {
                                                                                            // This is a channel with valid GPS signal
                                                                                            gps_eph_iter = d_user_pvt_solver->gps_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (gps_eph_iter != d_user_pvt_solver->gps_ephemeris_map.cend())
                                                                                                {
                                                                                                    gps_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                            if (gal_channel == 0)
                                                                                {
                                                                                    if (system == "E")
                                                                                        {
                                                                                            gal_eph_iter = d_user_pvt_solver->galileo_ephemeris_map.find(gnss_observables_iter.second.PRN);
                                                                                            if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                                                {
                                                                                                    gal_channel = 1;
                                                                                                }
                                                                                        }
                                                                                }
                                                                        }
                                                                    if (gps_eph_iter != d_user_pvt_solver->gps_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, gps_eph_iter->second, {}, {}, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                    if (gal_eph_iter != d_user_pvt_solver->galileo_ephemeris_map.cend())
                                                                        {
                                                                            d_rtcm_printer->Print_Rtcm_MSM(7, {}, {}, gal_eph_iter->second, {}, d_rx_time, d_gnss_observables_map, d_enable_rx_clock_correction, 0, 0, false, false);
                                                                        }
                                                                }
                                                            d_rtcm_writing_started = true;
                                                            break;
                                                        default:
                                                            break;
                                                        }
                                                }
                                        }
                                    catch (const boost::exception& ex)
                                        {
                                            std::cout << "RTCM boost exception: " << boost::diagnostic_information(ex) << '\n';
                                            LOG(ERROR) << "RTCM boost exception: " << boost::diagnostic_information(ex);
                                        }
                                    catch (const std::exception& ex)
                                        {
                                            std::cout << "RTCM std exception: " << ex.what() << '\n';
                                            LOG(ERROR) << "RTCM std exception: " << ex.what();
                                        }
                                }
                        }

                    // DEBUG MESSAGE: Display position in console output
                    if (d_user_pvt_solver->is_valid_position() and flag_display_pvt)
                        {
                            boost::posix_time::ptime time_solution;
                            std::string UTC_solution_str;
                            if (d_show_local_time_zone)
                                {
                                    time_solution = d_user_pvt_solver->get_position_UTC_time() + d_utc_diff_time;
                                    UTC_solution_str = d_local_time_str;
                                }
                            else
                                {
                                    time_solution = d_user_pvt_solver->get_position_UTC_time();
                                    UTC_solution_str = " UTC";
                                }
                            std::streamsize ss = std::cout.precision();  // save current precision
                            std::cout.setf(std::ios::fixed, std::ios::floatfield);
                            auto facet = new boost::posix_time::time_facet("%Y-%b-%d %H:%M:%S.%f %z");
                            std::cout.imbue(std::locale(std::cout.getloc(), facet));
                            std::cout
                                << TEXT_BOLD_GREEN
                                << "Position at " << time_solution << UTC_solution_str
                                << " using " << d_user_pvt_solver->get_num_valid_observations()
                                << std::fixed << std::setprecision(9)
                                << " observations is Lat = " << d_user_pvt_solver->get_latitude() << " [deg], Long = " << d_user_pvt_solver->get_longitude()
                                << std::fixed << std::setprecision(3)
                                << " [deg], Height = " << d_user_pvt_solver->get_height() << " [m]" << TEXT_RESET << '\n';

                            std::cout << std::setprecision(ss);
			    if (d_PPS_correction)
			        {
				    if (d_PPS_estimator_selected)
				        {
                            		    d_estimator_prev=d_estimator;
			    		    d_pps_offset=d_internal_pvt_solver->get_time_offset_s();
                            		    d_estimator=d_estimator*0.9+5.5e-8*(d_LO_external_frequ-d_LO_external_frequ_init)+0.1*(d_pps_offset-d_pps_init_offset);
			    		    d_FREQU_correction=d_estimator*(d_PPS_Kp+d_PPS_Ki)-d_estimator_prev*d_PPS_Kp;
					}
				    else
					{
			    		    d_pps_prev_error=d_pps_offset-d_pps_init_offset;
			    		    d_pps_offset=d_internal_pvt_solver->get_time_offset_s();
			    		    d_FREQU_correction=(d_pps_offset-d_pps_init_offset)*(d_PPS_Kp+d_PPS_Ki)-d_pps_prev_error*d_PPS_Kp;
					}
				    if (d_FREQU_correction>0.074) //max 0.074 Hz
					{
					    d_FREQU_correction=0.074;
					    std::cout << "sat\n";
					}
			    	    if (d_FREQU_correction<-0.074) //max 0.074 Hz
					{
					    d_FREQU_correction=-0.074;
					    std::cout << "SAT\n";
					}
		    		    std::cout
					<< "Estimator selected: " << std::boolalpha << d_PPS_estimator_selected  << " value: " <<  d_estimator << '\n';
			    		    std::cout
					<< std::fixed << std::setprecision(12)
                               		<< "RX clock offset: " << d_pps_offset << " [s] " 
					<< "diff offset: " << d_pps_offset-d_pps_init_offset << " [s] " 
					<< "Frequ correction: " << d_FREQU_correction << " [Hz]" << '\n';

                            	    char cmd[256];
			    	    d_LO_external_frequ=d_LO_external_frequ-d_FREQU_correction; 

				    std::cout
					<< std::fixed << std::setprecision(3)
					<< "LO Frequency: " << d_LO_external_frequ << " [Hz]" << '\n';

    			    	    sprintf(cmd, "FREQ %0.3fHz",d_LO_external_frequ);
			    	    vxi11_send (d_clink, cmd,strlen(cmd));
			        }
                            std::cout
                                << TEXT_BOLD_GREEN
                                << "Velocity: " << std::fixed << std::setprecision(3)
                                << "East: " << d_user_pvt_solver->get_rx_vel()[0] << " [m/s], North: " << d_user_pvt_solver->get_rx_vel()[1]
                                << " [m/s], Up = " << d_user_pvt_solver->get_rx_vel()[2] << " [m/s]" << TEXT_RESET << '\n';

                            std::cout << std::setprecision(ss);
                            DLOG(INFO) << "RX clock drift: " << d_user_pvt_solver->get_clock_drift_ppm() << " [ppm]";

                            // boost::posix_time::ptime p_time;
                            // gtime_t rtklib_utc_time = gpst2time(adjgpsweek(d_user_pvt_solver->gps_ephemeris_map.cbegin()->second.i_GPS_week), d_rx_time);
                            // p_time = boost::posix_time::from_time_t(rtklib_utc_time.time);
                            // p_time += boost::posix_time::microseconds(round(rtklib_utc_time.sec * 1e6));
                            // std::cout << TEXT_MAGENTA << "Observable RX time (GPST) " << boost::posix_time::to_simple_string(p_time) << TEXT_RESET << '\n';

                            DLOG(INFO) << "Position at " << boost::posix_time::to_simple_string(d_user_pvt_solver->get_position_UTC_time())
                                       << " UTC using " << d_user_pvt_solver->get_num_valid_observations() << " observations is Lat = " << d_user_pvt_solver->get_latitude() << " [deg], Long = " << d_user_pvt_solver->get_longitude()
                                       << " [deg], Height = " << d_user_pvt_solver->get_height() << " [m]";

                            /* std::cout << "Dilution of Precision at " << boost::posix_time::to_simple_string(d_user_pvt_solver->get_position_UTC_time())
                                         << " UTC using "<< d_user_pvt_solver->get_num_valid_observations() <<" observations is HDOP = " << d_user_pvt_solver->get_hdop() << " VDOP = "
                                         << d_user_pvt_solver->get_vdop()
                                         << " GDOP = " << d_user_pvt_solver->get_gdop() << '\n'; */
                        }

                    // PVT MONITOR
                    if (d_user_pvt_solver->is_valid_position())
                        {
                            const std::shared_ptr<Monitor_Pvt> monitor_pvt = std::make_shared<Monitor_Pvt>(d_user_pvt_solver->get_monitor_pvt());

                            // publish new position to the gnss_flowgraph channel status monitor
                            if (current_RX_time_ms % d_report_rate_ms == 0)
                                {
                                    this->message_port_pub(pmt::mp("status"), pmt::make_any(monitor_pvt));
                                }
                            if (d_flag_monitor_pvt_enabled)
                                {
                                    d_udp_sink_ptr->write_monitor_pvt(monitor_pvt.get());
                                }
                        }
                }
        }

    return noutput_items;
}
