/*!
 * \file gps_l1_ca_dll_pll_tracking_cc.cc
 * \brief Implementation of a code DLL + carrier PLL tracking block
 * \author Carlos Aviles, 2010. carlos.avilesr(at)googlemail.com
 *         Javier Arribas, 2011. jarribas(at)cttc.es
 *
 * Code DLL + carrier PLL according to the algorithms described in:
 * [1] K.Borre, D.M.Akos, N.Bertelsen, P.Rinder, and S.H.Jensen,
 * A Software-Defined GPS and Galileo Receiver. A Single-Frequency
 * Approach, Birkhauser, 2007
 *
 * -------------------------------------------------------------------------
 *
 * Copyright (C) 2010-2014  (see AUTHORS file for a list of contributors)
 *
 * GNSS-SDR is a software defined Global Navigation
 *          Satellite Systems receiver
 *
 * This file is part of GNSS-SDR.
 *
 * GNSS-SDR is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * GNSS-SDR is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNSS-SDR. If not, see <http://www.gnu.org/licenses/>.
 *
 * -------------------------------------------------------------------------
 */

#include "gps_l1_ca_dll_pll_CADLL_tracking_cc.h"
#include <cmath>
#include <iostream>
#include <memory>
#include <sstream>
#include <boost/lexical_cast.hpp>
#include <gnuradio/io_signature.h>
#include <glog/logging.h>
#include "gnss_synchro.h"
#include "gps_sdr_signal_processing.h"
#include "tracking_discriminators.h"
#include "lock_detectors.h"
#include "GPS_L1_CA.h"
#include "control_message_factory.h"


/*!
 * \todo Include in definition header file
 */
#define CN0_ESTIMATION_SAMPLES 20
#define MINIMUM_VALID_CN0 25
#define MAXIMUM_LOCK_FAIL_COUNTER 50
#define CARRIER_LOCK_THRESHOLD 0.85


using google::LogMessage;

gps_l1_ca_dll_pll_CADLL_tracking_cc_sptr
gps_l1_ca_dll_pll_CADLL_make_tracking_cc(
        long if_freq,
        long fs_in,
        unsigned int vector_length,
        boost::shared_ptr<gr::msg_queue> queue,
        bool dump,
        std::string dump_filename,
        float pll_bw_hz,
        float dll_bw_hz,
        float early_late_space_chips)
{
    return gps_l1_ca_dll_pll_CADLL_tracking_cc_sptr(new Gps_L1_Ca_Dll_Pll_CADLL_Tracking_cc(if_freq,
            fs_in, vector_length, queue, dump, dump_filename, pll_bw_hz, dll_bw_hz, early_late_space_chips));
}



void Gps_L1_Ca_Dll_Pll_CADLL_Tracking_cc::forecast (int noutput_items,
        gr_vector_int &ninput_items_required)
{
    ninput_items_required[0] = (int)d_vector_length*2; //set the required available samples in each call
}



Gps_L1_Ca_Dll_Pll_CADLL_Tracking_cc::Gps_L1_Ca_Dll_Pll_CADLL_Tracking_cc(
        long if_freq,
        long fs_in,
        unsigned int vector_length,
        boost::shared_ptr<gr::msg_queue> queue,
        bool dump,
        std::string dump_filename,
        float pll_bw_hz,
        float dll_bw_hz,
        float early_late_space_chips) :
        gr::block("Gps_L1_Ca_Dll_Pll_CADLL_Tracking_cc", gr::io_signature::make(1, 1, sizeof(gr_complex)),
                gr::io_signature::make(1, 1, sizeof(Gnss_Synchro)))
{
    // initialize internal vars
    d_queue = queue;
    d_dump = dump;
    d_if_freq = if_freq;
    d_fs_in = fs_in;
    d_vector_length = vector_length;
    d_dump_filename = dump_filename;

    // Initialize tracking  ==========================================
    d_code_loop_filter.set_DLL_BW(dll_bw_hz);
    d_code_loop_filter_m.set_DLL_BW(dll_bw_hz);
    d_amplitude_loop_filter.set_ALL_BW(10);
    d_amplitude_loop_filter_m.set_ALL_BW(10);
    d_carrier_loop_filter.set_PLL_BW(pll_bw_hz);

    //--- DLL variables --------------------------------------------------------
    d_early_late_spc_chips = early_late_space_chips; // Define early-late offset (in chips)

    // Initialization of local code replica
    // Get space for a vector with the C/A code replica sampled 1x/chip
    d_ca_code = new gr_complex[(int)GPS_L1_CA_CODE_LENGTH_CHIPS + 2];

    /* If an array is partitioned for more than one thread to operate on,
     * having the sub-array boundaries unaligned to cache lines could lead
     * to performance degradation. Here we allocate memory
     * (gr_complex array of size 2*d_vector_length) aligned to cache of N bytes (machine dependent!)
     */
    // Get space for the resampled early / prompt / late local replicas
    d_early_code=(gr_complex*)volk_malloc(2*d_vector_length * sizeof(gr_complex),volk_get_alignment());
    d_prompt_code=(gr_complex*)volk_malloc(2*d_vector_length * sizeof(gr_complex),volk_get_alignment());
    d_late_code=(gr_complex*)volk_malloc(2*d_vector_length * sizeof(gr_complex),volk_get_alignment());
    d_early_code_m=(gr_complex*)volk_malloc(2*d_vector_length * sizeof(gr_complex),volk_get_alignment());
    d_prompt_code_m=(gr_complex*)volk_malloc(2*d_vector_length * sizeof(gr_complex),volk_get_alignment());
    d_late_code_m=(gr_complex*)volk_malloc(2*d_vector_length * sizeof(gr_complex),volk_get_alignment());

    // space for carrier wipeoff and signal baseband vectors
    d_carr_sign=(gr_complex*)volk_malloc(2*d_vector_length * sizeof(gr_complex),volk_get_alignment());

    // correlator outputs (scalar)
    d_Early=(gr_complex*)volk_malloc(sizeof(gr_complex),volk_get_alignment());
    d_Prompt=(gr_complex*)volk_malloc(sizeof(gr_complex),volk_get_alignment());
    d_Late=(gr_complex*)volk_malloc(sizeof(gr_complex),volk_get_alignment());
    d_Early_m=(gr_complex*)volk_malloc(sizeof(gr_complex),volk_get_alignment());
    d_Prompt_m=(gr_complex*)volk_malloc(sizeof(gr_complex),volk_get_alignment());
    d_Late_m=(gr_complex*)volk_malloc(sizeof(gr_complex),volk_get_alignment());


    //--- Perform initializations ------------------------------
    // define initial code frequency basis of NCO
    d_code_freq_chips = GPS_L1_CA_CODE_RATE_HZ;
    // define residual code phase (in chips)
    d_rem_code_phase_samples = 0.0;
    d_rem_code_phase_samples_m = 0.0;
    // define residual carrier phase
    d_rem_carr_phase_rad = 0.0;

    // sample synchronization
    d_sample_counter = 0;
    d_sample_counter_m = 0;
    d_sample_counter_f = 0;
    d_sample_counter_fm = 0;
    //d_sample_counter_seconds = 0;
    d_acq_sample_stamp = 0;

    d_enable_tracking = false;
    d_pull_in = false;
    d_last_seg = 0;

    d_current_prn_length_samples = (int)d_vector_length;
    d_current_prn_length_samples_m = (int)d_vector_length;

    // CN0 estimation and lock detector buffers
    d_cn0_estimation_counter = 0;
    d_Prompt_buffer = new gr_complex[CN0_ESTIMATION_SAMPLES];
    d_carrier_lock_test = 1;
    d_CN0_SNV_dB_Hz = 0;
    d_carrier_lock_fail_counter = 0;
    d_carrier_lock_threshold = CARRIER_LOCK_THRESHOLD;

    systemName["G"] = std::string("GPS");
    systemName["R"] = std::string("GLONASS");
    systemName["S"] = std::string("SBAS");
    systemName["E"] = std::string("Galileo");
    systemName["C"] = std::string("Compass");
}

void Gps_L1_Ca_Dll_Pll_CADLL_Tracking_cc::stop_tracking()
    {
        DLOG(INFO) << "stopped tracking";
        std::cout << "stopped tracking" << std::endl;
        ControlMessageFactory* cmf = new ControlMessageFactory();
        if (d_queue != gr::msg_queue::sptr())
        {
            d_queue->handle(cmf->GetQueueMessage(d_channel, 2));
        }
        delete cmf;
        d_enable_tracking = false; 
    }


void Gps_L1_Ca_Dll_Pll_CADLL_Tracking_cc::start_tracking()
{
    /*
     *  correct the code phase according to the delay between acq and trk
     */
    d_acq_code_phase_samples = d_acquisition_gnss_synchro->Acq_delay_samples;
    d_acq_carrier_doppler_hz = d_acquisition_gnss_synchro->Acq_doppler_hz;
    d_acq_sample_stamp =  d_acquisition_gnss_synchro->Acq_samplestamp_samples;

    long int acq_trk_diff_samples;
    float acq_trk_diff_seconds;
    acq_trk_diff_samples = (long int)d_sample_counter - (long int)d_acq_sample_stamp;//-d_vector_length;
    LOG(INFO) << "Number of samples between Acquisition and Tracking =" << acq_trk_diff_samples;
    acq_trk_diff_seconds = (float)acq_trk_diff_samples / (float)d_fs_in;
    //doppler effect
    // Fd=(C/(C+Vr))*F
    float radial_velocity;
    radial_velocity = (GPS_L1_FREQ_HZ + d_acq_carrier_doppler_hz)/GPS_L1_FREQ_HZ;
    // new chip and prn sequence periods based on acq Doppler
    float T_chip_mod_seconds;
    float T_prn_mod_seconds;
    float T_prn_mod_samples;
    d_code_freq_chips = radial_velocity * GPS_L1_CA_CODE_RATE_HZ;
    T_chip_mod_seconds = 1/d_code_freq_chips;
    T_prn_mod_seconds = T_chip_mod_seconds * GPS_L1_CA_CODE_LENGTH_CHIPS;
    T_prn_mod_samples = T_prn_mod_seconds * (float)d_fs_in;

    d_current_prn_length_samples = round(T_prn_mod_samples);
    d_current_prn_length_samples_m = round(T_prn_mod_samples);

    float T_prn_true_seconds = GPS_L1_CA_CODE_LENGTH_CHIPS / GPS_L1_CA_CODE_RATE_HZ;
    float T_prn_true_samples = T_prn_true_seconds * (float)d_fs_in;
    float T_prn_diff_seconds;
    T_prn_diff_seconds = T_prn_true_seconds - T_prn_mod_seconds;
    float N_prn_diff;
    N_prn_diff = acq_trk_diff_seconds / T_prn_true_seconds;
    float corrected_acq_phase_samples, delay_correction_samples;
    corrected_acq_phase_samples = fmod((d_acq_code_phase_samples + T_prn_diff_seconds * N_prn_diff * (float)d_fs_in), T_prn_true_samples);
    if (corrected_acq_phase_samples < 0)
        {
            corrected_acq_phase_samples = T_prn_mod_samples + corrected_acq_phase_samples;
        }
    delay_correction_samples = d_acq_code_phase_samples - corrected_acq_phase_samples;

    d_acq_code_phase_samples = corrected_acq_phase_samples;

    d_carrier_doppler_hz = d_acq_carrier_doppler_hz;

    // DLL/PLL filter initialization
    d_carrier_loop_filter.initialize(); // initialize the carrier filter
    d_code_loop_filter.initialize();    // initialize the code filter
    d_code_loop_filter_m.initialize();    // initialize the code filter
    d_amplitude_loop_filter.initialize();    // initialize the amplitude filter
    d_amplitude_loop_filter_m.initialize();    // initialize the amplitude filter
    d_integrator.initialize();    
    d_integrator_m.initialize();    


    d_carrier_lock_fail_counter = 0;
    d_rem_code_phase_samples = 0;
    d_rem_code_phase_samples_m = 0;
    d_rem_carr_phase_rad = 0;
    d_acc_carrier_phase_rad = 0;
    d_acc_code_phase_secs = 0;
    d_acc_code_phase_secs = 0;

    d_code_phase_samples = d_acq_code_phase_samples;
    // generate local reference ALWAYS starting at chip 1 (1 sample per chip)
    gps_l1_ca_code_gen_complex(&d_ca_code[1], d_acquisition_gnss_synchro->PRN, 0);
    d_ca_code[0] = d_ca_code[(int)GPS_L1_CA_CODE_LENGTH_CHIPS];
    d_ca_code[(int)GPS_L1_CA_CODE_LENGTH_CHIPS + 1] = d_ca_code[1];

    std::string sys_ = &d_acquisition_gnss_synchro->System;
    sys = sys_.substr(0,1);

    // DEBUG OUTPUT
    std::cout << "Tracking start on channel " << d_channel << " for satellite " << Gnss_Satellite(systemName[sys], d_acquisition_gnss_synchro->PRN) << std::endl;
    LOG(INFO) << "Starting tracking of satellite " << Gnss_Satellite(systemName[sys], d_acquisition_gnss_synchro->PRN) << " on channel " << d_channel;


    // enable tracking
    d_pull_in = true;
    d_enable_tracking = true;

    LOG(INFO) << "PULL-IN Doppler [Hz]=" << d_carrier_doppler_hz
            << " Code Phase correction [samples]=" << delay_correction_samples
            << " PULL-IN Code Phase [samples]=" << d_acq_code_phase_samples;
}




void Gps_L1_Ca_Dll_Pll_CADLL_Tracking_cc::update_local_code_m()
{
    double tcode_chips;
    double rem_code_phase_chips;
    int associated_chip_index;
    int code_length_chips = (int)GPS_L1_CA_CODE_LENGTH_CHIPS;
    double code_phase_step_chips;
    int early_late_spc_samples;
    int epl_loop_length_samples;

    // unified loop for E, P, L code vectors
    code_phase_step_chips = ((double)d_code_freq_chips) / ((double)d_fs_in);
    rem_code_phase_chips = (d_rem_code_phase_samples_m) * (d_code_freq_chips / d_fs_in);
    tcode_chips = -rem_code_phase_chips;
    //std::cout << "tcode_chips m " << tcode_chips << std::endl;

    // Alternative EPL code generation (40% of speed improvement!)
    early_late_spc_samples = round(d_early_late_spc_chips / code_phase_step_chips);
    epl_loop_length_samples = d_current_prn_length_samples_m + early_late_spc_samples*2;
    for (int i = 0; i < epl_loop_length_samples; i++)
        {
            associated_chip_index = 1 + round(fmod(tcode_chips - d_early_late_spc_chips, code_length_chips));
            if(associated_chip_index < 0 )
                associated_chip_index = 1023+associated_chip_index;

            //DLOG(INFO) << "d_early_late_spc_chips " << d_early_late_spc_chips << " aci " << d_ca_code[associated_chip_index];
            d_early_code_m[i] = d_ca_code[associated_chip_index];
            //std::cout << "code " << d_early_code_m[i] << std::endl;

            tcode_chips = tcode_chips + code_phase_step_chips;
        }

    memcpy(d_prompt_code_m,&d_early_code_m[early_late_spc_samples],d_current_prn_length_samples_m* sizeof(gr_complex));
    memcpy(d_late_code_m,&d_early_code_m[early_late_spc_samples*2],d_current_prn_length_samples_m* sizeof(gr_complex));
}


void Gps_L1_Ca_Dll_Pll_CADLL_Tracking_cc::update_local_code()
{
    double tcode_chips;
    double rem_code_phase_chips;
    int associated_chip_index;
    int code_length_chips = (int)GPS_L1_CA_CODE_LENGTH_CHIPS;
    double code_phase_step_chips;
    int early_late_spc_samples;
    int epl_loop_length_samples;

    // unified loop for E, P, L code vectors
    code_phase_step_chips = ((double)d_code_freq_chips) / ((double)d_fs_in);
    rem_code_phase_chips = d_rem_code_phase_samples * (d_code_freq_chips / d_fs_in);
    tcode_chips = -rem_code_phase_chips;
    //std::cout << "tcode_chips " << tcode_chips << std::endl;

    // Alternative EPL code generation (40% of speed improvement!)
    early_late_spc_samples = round(d_early_late_spc_chips / code_phase_step_chips);
    epl_loop_length_samples = d_current_prn_length_samples + early_late_spc_samples*2;
    for (int i = 0; i < epl_loop_length_samples; i++)
        {
            associated_chip_index = 1 + round(fmod(tcode_chips - d_early_late_spc_chips, code_length_chips));
            //std::cout << "aci " << associated_chip_index << std::endl;
            d_early_code[i] = d_ca_code[associated_chip_index];
    //        std::cout << "early a: " << d_early_code[i] << std::endl;
            tcode_chips = tcode_chips + code_phase_step_chips;
        }

    memcpy(d_prompt_code,&d_early_code[early_late_spc_samples],d_current_prn_length_samples* sizeof(gr_complex));
    memcpy(d_late_code,&d_early_code[early_late_spc_samples*2],d_current_prn_length_samples* sizeof(gr_complex));
}




void Gps_L1_Ca_Dll_Pll_CADLL_Tracking_cc::update_local_carrier()
{
    float phase_rad, phase_step_rad;

    phase_step_rad = (float)GPS_TWO_PI*d_carrier_doppler_hz / (float)d_fs_in;
    phase_rad = d_rem_carr_phase_rad;
    for(int i = 0; i < d_current_prn_length_samples; i++)
        {
            d_carr_sign[i] = gr_complex(cos(phase_rad), -sin(phase_rad));
            phase_rad += phase_step_rad;
        }
    //d_rem_carr_phase_rad = fmod(phase_rad, GPS_TWO_PI);
    //d_acc_carrier_phase_rad = d_acc_carrier_phase_rad + d_rem_carr_phase_rad;
}




Gps_L1_Ca_Dll_Pll_CADLL_Tracking_cc::~Gps_L1_Ca_Dll_Pll_CADLL_Tracking_cc()
{
    d_dump_file.close();

    volk_free(d_prompt_code);
    volk_free(d_late_code);
    volk_free(d_early_code);
    volk_free(d_carr_sign);
    volk_free(d_Early);
    volk_free(d_Prompt);
    volk_free(d_Late);

    volk_free(d_prompt_code_m);
    volk_free(d_late_code_m);
    volk_free(d_early_code_m);
    volk_free(d_Early_m);
    volk_free(d_Prompt_m);
    volk_free(d_Late_m);

    delete[] d_ca_code;
    delete[] d_Prompt_buffer;
}



int Gps_L1_Ca_Dll_Pll_CADLL_Tracking_cc::general_work (int noutput_items, gr_vector_int &ninput_items,
        gr_vector_const_void_star &input_items, gr_vector_void_star &output_items)
{
    // process vars
    float carr_error_hz;
    float carr_error_filt_hz;
    float code_error_chips;
    float code_error_chips_m;
    float code_error_filt_chips;
    float code_error_filt_chips_m;



    if (d_enable_tracking == true)
        {
            // Receiver signal alignment
            if (d_pull_in == true)
                {
                    int samples_offset;
                    float acq_trk_shif_correction_samples;
                    int acq_to_trk_delay_samples;
                    acq_to_trk_delay_samples = d_sample_counter - d_acq_sample_stamp;
                    acq_trk_shif_correction_samples = d_current_prn_length_samples - fmod((float)acq_to_trk_delay_samples, (float)d_current_prn_length_samples);
                    samples_offset = round(d_acq_code_phase_samples + acq_trk_shif_correction_samples);
                    // /todo: Check if the sample counter sent to the next block as a time reference should be incremented AFTER sended or BEFORE
                    //d_sample_counter_seconds = d_sample_counter_seconds + (((double)samples_offset) / (double)d_fs_in);
                    d_sample_counter = d_sample_counter + samples_offset; //count for the processed samples
                    d_pull_in = false;
                    //std::cout<<" samples_offset="<<samples_offset<<"\r\n";
                    d_sample_counter_m = d_sample_counter;
                    d_sample_counter_f = d_sample_counter;
                    d_sample_counter_fm = d_sample_counter;
                    consume_each(samples_offset); //shift input to perform alignment with local replica
                    return 1;
                }

            // GNSS_SYNCHRO OBJECT to interchange data between tracking->telemetry_decoder
            Gnss_Synchro current_synchro_data;
            // Fill the acquisition data
            current_synchro_data = *d_acquisition_gnss_synchro;

            // Block input data and block output stream pointers
            const gr_complex* in = (gr_complex*) input_items[0]; //PRN start block alignment
            Gnss_Synchro **out = (Gnss_Synchro **) &output_items[0];


            float corr = 1.0;
            float corr_m;
            if(CADLL_init)
                {

                    // Generate local code and carrier replicas (using \hat{f}_d(k-1))
                    update_local_code();
                    update_local_code_m();
                    update_local_carrier();

                    // perform carrier wipe-off and compute Early, Prompt and Late correlation
                    d_correlator.Carrier_wipeoff_and_EPL_volk(d_current_prn_length_samples,
                            in,
                            d_carr_sign,
                            d_early_code,
                            d_prompt_code,
                            d_late_code,
                            d_Early,
                            d_Prompt,
                            d_Late,
                            is_unaligned());
                }
            else
                {
                    // Generate local code and carrier replicas (using \hat{f}_d(k-1))
                    update_local_code();
                    update_local_code_m();
                    update_local_carrier();



                    // perform carrier wipe-off and compute Early, Prompt and Late correlation
                    d_correlator.Carrier_wipeoff_and_EPL_CADLL_volk(d_current_prn_length_samples,
                            in,
                            d_carr_sign,
                            d_early_code,
                            d_prompt_code,
                            d_late_code,
                            d_Early,
                            d_Prompt,
                            d_Late,
                            d_early_code_m,
                            d_prompt_code_m,
                            d_late_code_m,
                            d_Early_m,
                            d_Prompt_m,
                            d_Late_m,
                            a1,
                            a2,
                            &corr,
                            &corr_m,
                            is_unaligned());

                }
    
               
            //std::cout << "after w: " << *d_Prompt << " " << *d_Early << " " << *d_Late  << " " << d_current_prn_length_samples << std::endl;
            
            // check for samples consistency (this should be done before in the receiver / here only if the source is a file)
            if (std::isnan((*d_Prompt).real()) == true or std::isnan((*d_Prompt).imag()) == true ) // or std::isinf(in[i].real())==true or std::isinf(in[i].imag())==true)
                {
                    const int samples_available = ninput_items[0];
                    d_sample_counter = d_sample_counter + samples_available;
                    LOG(WARNING) << "Detected NaN samples at sample number " << d_sample_counter;
                    consume_each(samples_available);

                    // make an output to not stop the rest of the processing blocks
                    current_synchro_data.Prompt_I = 0.0;
                    current_synchro_data.Prompt_Q = 0.0;
                    current_synchro_data.Tracking_timestamp_secs = (double)d_sample_counter/(double)d_fs_in;
                    current_synchro_data.Carrier_phase_rads = 0.0;
                    current_synchro_data.Code_phase_secs = 0.0;
                    current_synchro_data.CN0_dB_hz = 0.0;
                    current_synchro_data.Flag_valid_tracking = false;

                    *out[0] = current_synchro_data;

                    return 1;
                }
            /*
            *d_Early = gr_complex(d_code_loop_filter_E.get_code_nco(d_Early->real()), d_code_loop_filter_Eq.get_code_nco(d_Early->imag()));
            *d_Prompt = gr_complex(d_code_loop_filter_P.get_code_nco(d_Prompt->real()), d_code_loop_filter_Pq.get_code_nco(d_Prompt->imag()));
            *d_Late = gr_complex(d_code_loop_filter_L.get_code_nco(d_Late->real()), d_code_loop_filter_Lq.get_code_nco(d_Late->imag()));
            *d_Early_m = gr_complex(d_code_loop_filter_Em.get_code_nco(d_Early_m->real()), d_code_loop_filter_Emq.get_code_nco(d_Early_m->imag()));
            *d_Prompt_m = gr_complex(d_code_loop_filter_Pm.get_code_nco(d_Prompt_m->real()), d_code_loop_filter_Pmq.get_code_nco(d_Prompt_m->imag()));
            *d_Late_m = gr_complex(d_code_loop_filter_Lm.get_code_nco(d_Late_m->real()), d_code_loop_filter_Lmq.get_code_nco(d_Late_m->imag()));
            */
            //std::cout << "after f: " << *d_Prompt << " " << *d_Early << " " << *d_Late  << " " << d_current_prn_length_samples << std::endl;

            // ################## PLL ##########################################################
            // PLL discriminator
            carr_error_hz = pll_cloop_two_quadrant_atan(*d_Prompt) / (float)GPS_TWO_PI;
            // Carrier discriminator filter
            carr_error_filt_hz = d_carrier_loop_filter.get_carrier_nco(carr_error_hz);
            // New carrier Doppler frequency estimation
            d_carrier_doppler_hz = d_acq_carrier_doppler_hz + carr_error_filt_hz;
            // New code Doppler frequency estimation
            d_code_freq_chips = GPS_L1_CA_CODE_RATE_HZ + ((d_carrier_doppler_hz * GPS_L1_CA_CODE_RATE_HZ) / GPS_L1_FREQ_HZ);
            //carrier phase accumulator for (K) doppler estimation
            d_acc_carrier_phase_rad = d_acc_carrier_phase_rad + GPS_TWO_PI * d_carrier_doppler_hz * GPS_L1_CA_CODE_PERIOD;
            //remanent carrier phase to prevent overflow in the code NCO
            d_rem_carr_phase_rad = d_rem_carr_phase_rad + GPS_TWO_PI * d_carrier_doppler_hz * GPS_L1_CA_CODE_PERIOD;
            d_rem_carr_phase_rad = fmod(d_rem_carr_phase_rad, GPS_TWO_PI);



            // ################## DLL1 ##########################################################
            // DLL discriminator
            //std::cout << "early, late: " << *d_Early << " " << *d_Late << std::endl;
            //std::cout << "early, late m: " << *d_Early_m << " " << *d_Late_m << std::endl;
            //code_error_chips = dll_c_e_minus_l_normalized(*d_Early, *d_Late, *d_Prompt); //[chips/Ti]
            code_error_chips = dll_nc_e_minus_l_normalized(*d_Early, *d_Late); //[chips/Ti]
            // Code discriminator filter
            code_error_filt_chips = d_code_loop_filter.get_code_nco(code_error_chips); //[chips/second]
            //Code phase accumulator
            float code_error_filt_secs;
            code_error_filt_secs = (GPS_L1_CA_CODE_PERIOD * code_error_filt_chips) / GPS_L1_CA_CODE_RATE_HZ; //[seconds]
            d_acc_code_phase_secs = d_acc_code_phase_secs + code_error_filt_secs;

            // ################## DLL2 ##########################################################
            // DLL discriminator
            float code_error_filt_secs_m;
            if(!CADLL_init)
                {
                    //code_error_chips_m = dll_c_e_minus_l_normalized(*d_Early_m, *d_Late_m, *d_Prompt_m); //[chips/Ti]
                    code_error_chips_m = dll_nc_e_minus_l_normalized(*d_Early_m, *d_Late_m); //[chips/Ti]
                    // Code discriminator filter
                    code_error_filt_chips_m = d_code_loop_filter_m.get_code_nco(code_error_chips_m); //[chips/second]
                    //Code phase accumulator
                    code_error_filt_secs_m = (GPS_L1_CA_CODE_PERIOD * code_error_filt_chips_m) / GPS_L1_CA_CODE_RATE_HZ; //[seconds]
                    d_acc_code_phase_secs_m = d_acc_code_phase_secs_m + code_error_filt_secs_m;
                }


            // ################## CARRIER AND CODE NCO BUFFER ALIGNEMENT #######################
            // keep alignment parameters for the next input buffer
            double T_chip_seconds;
            double T_prn_seconds;
            double T_prn_samples;
            double K_blk_samples;
            double K_blk_samples_m;
            // Compute the next buffer length based in the new period of the PRN sequence and the code phase error estimation
            T_chip_seconds = 1 / (double)d_code_freq_chips;
            T_prn_seconds = T_chip_seconds * GPS_L1_CA_CODE_LENGTH_CHIPS;
            T_prn_samples = T_prn_seconds * (double)d_fs_in;
            K_blk_samples = T_prn_samples + d_rem_code_phase_samples + code_error_filt_secs * (double)d_fs_in;
                
            if(CADLL_init)
                {
                    K_blk_samples_m = K_blk_samples;
                    d_rem_code_phase_samples_m = d_rem_code_phase_samples;
                }
            else
                {
                    K_blk_samples_m = T_prn_samples + d_rem_code_phase_samples_m + (code_error_filt_secs_m) * (double)d_fs_in;
                }

            //std::cout << "K_blk: " << K_blk_samples << " " << K_blk_samples_m << std::endl;
            //std::cout << "code_error_filt_secs: " << code_error_filt_secs << " " << code_error_filt_secs_m<< std::endl;
            /*
            std::cout << "code_error_chips: " << code_error_chips<< " " << code_error_chips_m<< std::endl;
            std::cout << "code_error_filt_chips: " << code_error_filt_chips<< " " << code_error_filt_chips_m<< std::endl;
            std::cout << "d_rem_code_phase_samples: " << d_rem_code_phase_samples<< " " << d_rem_code_phase_samples_m 
                        << " " << (code_error_filt_secs_m) * (double)d_fs_in 
                        << " " << (code_error_filt_secs) * (double)d_fs_in<< std::endl;

*/
            d_current_prn_length_samples = round(K_blk_samples); //round to a discrete samples
            d_current_prn_length_samples_m = round(K_blk_samples_m); //round to a discrete samples
        /*
            if(d_current_prn_length_samples_m > d_current_prn_length_samples)
                {
                    d_current_prn_length_samples = d_current_prn_length_samples_m;
                }
            else
                {*/
             //   }

            d_sample_counter_f += K_blk_samples;
            d_sample_counter_fm += K_blk_samples_m; 
            //d_current_prn_length_samples_m = d_current_prn_length_samples;
/*
            double sum = 0.0;
            double d_input_power;
            
            const gr_complex* in_ = in;
            for(int i = 0; i < d_current_prn_length_samples; ++i)
                {
                    sum += (*in_++).real();
                    //sum += std::abs(*in_++);
                }

            d_input_power = sum / (double)d_current_prn_length_samples;
            */
            
            float d_input_power = corr;
            // ################## ALL ##########################################################
            float a1_, a2_;
            float lambda  = d_current_prn_length_samples;
            float f_a1, f_a2;
            //float lambda  = 0.99;
            //float lambda  = 20e3;
            double Z, Z_m;
            if(CADLL_init)
                {
                    //a1_ = all(*d_Prompt, lambda); 
                    //a1_ = all(*d_Early, *d_Late, *d_Prompt, lambda); //[chips/Ti]
                    a1_ = d_input_power/0.99;
                    f_a1 = d_amplitude_loop_filter.get_amplitude_nco(a1_);
                    a1 = f_a1;
                    
                    //a1 = a1 +(a1_-a1); 
                   // a1 = a1+(a1_-a1)*0.1;
                    a2 = a1/1.284025416687741;
                }
            else
                {
                    double t = (*d_Prompt).real()+(*d_Prompt_m).real(); 
                    double d = d_input_power/t;
                    Z = d*(*d_Prompt).real();
                    Z_m = d*(*d_Prompt_m).real();

                    //a1_ = all(*d_Prompt, lambda); 
                  //  a1_ = all(*d_Early, *d_Late, *d_Prompt, lambda); //[chips/Ti]
                    //a2_ = all(*d_Prompt_m, lambda); 
                    a1_ = Z/0.99;
                    a2_ = Z_m/0.99;
                    //a2_ = all(*d_Early_m, *d_Late_m, *d_Prompt_m, lambda); //[chips/Ti]
                    f_a1 = d_amplitude_loop_filter.get_amplitude_nco(a1_);
                    a1 = f_a1;
                    f_a2 = d_amplitude_loop_filter_m.get_amplitude_nco(a2_);
                    a2 = f_a2;
                
                //    a1 = a1_;// + (a1_-a1); 
                 //   a2 = a2_;// + (a2_-a2);
                    //a1 = a1+(a1_-a1)*0.1;
                    //a2 = a2+(a2_-a2)*0.1;
                }
            /*
            std::cout << "all1: " << *d_Prompt << " " << *d_Early << " " << *d_Late  << " " << d_current_prn_length_samples << std::endl;
            std::cout << "all2: " << *d_Prompt_m << " " << *d_Early_m << " " << *d_Late_m <<std::endl;
            std::cout << "code: " << *d_prompt_code << " " << *d_prompt_code_m << " " << *in <<std::endl;
            std::cout << "a1_ " << a1_ << " " <<  f_a1 << std::endl;
            std::cout << "a2_ " << a2_ << " " << f_a2 << std::endl;
            std::cout << "a1 " << a1 << " a2: "<< a2 << std::endl;
*/

            // ####### CN0 ESTIMATION AND LOCK DETECTORS ######
            if (d_cn0_estimation_counter < CN0_ESTIMATION_SAMPLES)
                {
                    // fill buffer with prompt correlator output values
                    d_Prompt_buffer[d_cn0_estimation_counter] = *d_Prompt;
                    d_cn0_estimation_counter++;
                }
            else
                {
                    d_cn0_estimation_counter = 0;
                    // Code lock indicator
                    d_CN0_SNV_dB_Hz = cn0_svn_estimator(d_Prompt_buffer, CN0_ESTIMATION_SAMPLES, d_fs_in, GPS_L1_CA_CODE_LENGTH_CHIPS);
                    // Carrier lock indicator
                    d_carrier_lock_test = carrier_lock_detector(d_Prompt_buffer, CN0_ESTIMATION_SAMPLES);
                    // Loss of lock detection
                    if (d_carrier_lock_test < d_carrier_lock_threshold or d_CN0_SNV_dB_Hz < MINIMUM_VALID_CN0)
                        {
                            d_carrier_lock_fail_counter++;
                        }
                    else
                        {
                            if (d_carrier_lock_fail_counter > 0) d_carrier_lock_fail_counter--;
                        }
                    if (d_carrier_lock_fail_counter > MAXIMUM_LOCK_FAIL_COUNTER)
                        {
                            std::cout << "Loss of lock in channel " << d_channel << "!" << std::endl;
                            LOG(INFO) << "Loss of lock in channel " << d_channel << "!";
                            std::unique_ptr<ControlMessageFactory> cmf(new ControlMessageFactory());
                            if (d_queue != gr::msg_queue::sptr())
                                {
                                    d_queue->handle(cmf->GetQueueMessage(d_channel, 2));
                                }
                            d_carrier_lock_fail_counter = 0;
                            d_enable_tracking = false; // TODO: check if disabling tracking is consistent with the channel state machine
                        }
                }
            // ########### Output the tracking data to navigation and PVT ##########
            current_synchro_data.Prompt_I = (double)(*d_Prompt).real();
            current_synchro_data.Prompt_Q = (double)(*d_Prompt).imag();

            // Tracking_timestamp_secs is aligned with the NEXT PRN start sample (Hybridization problem!)
            //compute remnant code phase samples BEFORE the Tracking timestamp
            //d_rem_code_phase_samples = K_blk_samples - d_current_prn_length_samples; //rounding error < 1 sample
            //current_synchro_data.Tracking_timestamp_secs = ((double)d_sample_counter + (double)d_current_prn_length_samples + (double)d_rem_code_phase_samples)/(double)d_fs_in;

            // Tracking_timestamp_secs is aligned with the CURRENT PRN start sample (Hybridization OK!, but some glitches??)
            current_synchro_data.Tracking_timestamp_secs = ((double)d_sample_counter + (double)d_rem_code_phase_samples)/(double)d_fs_in;
            
            //compute remnant code phase samples AFTER the Tracking timestamp
            d_rem_code_phase_samples = K_blk_samples - d_current_prn_length_samples; //rounding error < 1 sample
            d_rem_code_phase_samples_m = K_blk_samples_m - d_current_prn_length_samples; //rounding error < 1 sample

            if(CADLL_init && current_synchro_data.Tracking_timestamp_secs > 1)  
                {
                    CADLL_init = false;
                    std::cout << "diff f: " << ((((double)d_rem_code_phase_samples)-(double)d_rem_code_phase_samples_m)/d_fs_in)*0.3e9 << " [m]" << std::endl;
                    d_acc_code_phase_secs_m = d_acc_code_phase_secs;
                    d_rem_code_phase_samples_m = d_rem_code_phase_samples-27; 
                }

            //current_synchro_data.Tracking_timestamp_secs = ((double)d_sample_counter)/(double)d_fs_in;
            // This tracking block aligns the Tracking_timestamp_secs with the start sample of the PRN, thus, Code_phase_secs=0
            current_synchro_data.Code_phase_secs = 0;
            current_synchro_data.Carrier_phase_rads = (double)d_acc_carrier_phase_rad;
            current_synchro_data.Carrier_Doppler_hz = (double)d_carrier_doppler_hz;
            current_synchro_data.CN0_dB_hz = (double)d_CN0_SNV_dB_Hz;
            *out[0] = current_synchro_data;
            /*
                            std::cout << "E " << *d_Early << " " << *d_Early_m << std::endl;
                            std::cout << "P " << *d_Prompt << " " << *d_Prompt_m << std::endl;
                            std::cout << "L " << *d_Late << " " << *d_Late_m << std::endl;
                            std::cout << "a: " << a1 << " " << a2 << std::endl;
*/
            // ########## DEBUG OUTPUT
            /*!
             *  \todo The stop timer has to be moved to the signal source!
             */
            // debug: Second counter in channel 0
            if (d_channel == 0)
                {
                    if (floor(d_sample_counter / d_fs_in) != d_last_seg)
                        {
        /*
                            float Ptot = 0;
                            for (int i=0; i<CN0_ESTIMATION_SAMPLES; i++)
                            {
                                Ptot += d_Prompt_buffer[i].imag() * d_Prompt_buffer[i].imag() + d_Prompt_buffer[i].real() * d_Prompt_buffer[i].real();
                            }
                            Ptot = Ptot / (float)CN0_ESTIMATION_SAMPLES;
                            */
                            d_last_seg = floor(d_sample_counter / d_fs_in);
                            std::cout << "Current input signal time = " << d_last_seg << " [s]" << std::endl;
                            //std::cout << "unit 1: " << ((double)d_sample_counter + (double)d_rem_code_phase_samples)/(double)d_fs_in << std::endl;
                            float u1 = ((double)d_sample_counter + (double)d_rem_code_phase_samples)/(double)d_fs_in;
                            //std::cout << "unit 2: " << ((double)d_sample_counter + (double)d_rem_code_phase_samples_m)/(double)d_fs_in << std::endl;
                            float u2 = ((double)d_sample_counter_m + (double)d_rem_code_phase_samples_m)/(double)d_fs_in;
                            std::cout << "diff: " << ((((double)d_rem_code_phase_samples) - (double)d_rem_code_phase_samples_m)/d_fs_in)*0.3e9 << " [m]" << std::endl;
                            DLOG(INFO) << "diff: " << (u2-u1)*0.3e9 << " [m]";
                            //std::cout << "diff: " << (u1-u2)*0.3e9 << " [m]" << std::endl;
                            //std::cout << "sample " << (double) (d_sample_counter_fm) * 0.3e9 <<  std::endl;
                            /*
                            std::cout << "E " << *d_Early << " " << *d_Early_m << std::endl;
                            std::cout << "P " << *d_Prompt << " " << *d_Prompt_m << std::endl;
                            std::cout << "L " << *d_Late << " " << *d_Late_m << std::endl;
                            std::cout << "a: " << a1 << " " << a2 << std::endl;
                            */
                            
           //                 std::cout << "sat: " <<d_acquisition_gnss_synchro->PRN << " Ptot: " << Ptot << " SNR: " << d_CN0_SNV_dB_Hz << " [dB-Hz]" << std::endl; 
                            LOG(INFO) << "Tracking CH " << d_channel <<  ": Satellite " << Gnss_Satellite(systemName[sys], d_acquisition_gnss_synchro->PRN)
                                      << ", CN0 = " << d_CN0_SNV_dB_Hz << " [dB-Hz]" << std::endl 
                                      << ", lock=" << d_carrier_lock_test << ", sample counter=" << d_sample_counter << std::endl;
                            //if (d_last_seg==5) d_carrier_lock_fail_counter=500; //DEBUG: force unlock!
                        }
                }
            else
                {
                    if (floor(d_sample_counter / d_fs_in) != d_last_seg)
                        {
                            
                            /*
                            LOG(INFO) << "Tracking CH " << d_channel <<  ": Satellite " << Gnss_Satellite(systemName[sys], d_acquisition_gnss_synchro->PRN)
                                      << ", CN0 = " << d_CN0_SNV_dB_Hz << " [dB-Hz]" << ", lock = " << d_carrier_lock_test;
                            //std::cout<<"TRK CH "<<d_channel<<" Carrier_lock_test="<<d_carrier_lock_test<< std::endl;
                            float Ptot = 0;
                            for (int i=0; i<CN0_ESTIMATION_SAMPLES; i++)
                            {
                                Ptot += d_Prompt_buffer[i].imag() * d_Prompt_buffer[i].imag() + d_Prompt_buffer[i].real() * d_Prompt_buffer[i].real();
                            }
                            Ptot = Ptot / (float)CN0_ESTIMATION_SAMPLES;
                            */
                            d_last_seg = floor(d_sample_counter / d_fs_in);
                            //std::cout << "Current input signal time = " << d_last_seg << " [s]" << std::endl;
                            //std::cout << "sat: " <<d_acquisition_gnss_synchro->PRN << " Ptot: " << Ptot << " SNR: " << d_CN0_SNV_dB_Hz << " [dB-Hz]" << std::endl; 
                        }
                }
        }
    else
        {
            // ########## DEBUG OUTPUT (TIME ONLY for channel 0 when tracking is disabled)
            /*!
             *  \todo The stop timer has to be moved to the signal source!
             */
            // stream to collect cout calls to improve thread safety
            std::stringstream tmp_str_stream;
            if (floor(d_sample_counter / d_fs_in) != d_last_seg)
                {
                    d_last_seg = floor(d_sample_counter / d_fs_in);

                    if (d_channel == 0)
                        {
                            // debug: Second counter in channel 0
                            tmp_str_stream << "Current input signal time = " << d_last_seg << " [s]" << std::endl << std::flush;
                            std::cout << tmp_str_stream.rdbuf() << std::flush;
                        }
                }
            *d_Early = gr_complex(0,0);
            *d_Prompt = gr_complex(0,0);
            *d_Late = gr_complex(0,0);
            Gnss_Synchro **out = (Gnss_Synchro **) &output_items[0]; //block output streams pointer
            // GNSS_SYNCHRO OBJECT to interchange data between tracking->telemetry_decoder
            *out[0] = *d_acquisition_gnss_synchro;
        }

    if(d_dump)
        {
            // MULTIPLEXED FILE RECORDING - Record results to file
            float prompt_I;
            float prompt_Q;
            float tmp_E, tmp_P, tmp_L;
            float tmp_float;
            double tmp_double;
            prompt_I = (*d_Prompt).real();
            prompt_Q = (*d_Prompt).imag();
            tmp_E = std::abs<float>(*d_Early);
            tmp_P = std::abs<float>(*d_Prompt);
            tmp_L = std::abs<float>(*d_Late);
            try
            {
                    // EPR
                    d_dump_file.write((char*)&tmp_E, sizeof(float));
                    d_dump_file.write((char*)&tmp_P, sizeof(float));
                    d_dump_file.write((char*)&tmp_L, sizeof(float));
                    // PROMPT I and Q (to analyze navigation symbols)
                    d_dump_file.write((char*)&prompt_I, sizeof(float));
                    d_dump_file.write((char*)&prompt_Q, sizeof(float));
                    // PRN start sample stamp
                    //tmp_float=(float)d_sample_counter;
                    d_dump_file.write((char*)&d_sample_counter, sizeof(unsigned long int));
                    // accumulated carrier phase
                    d_dump_file.write((char*)&d_acc_carrier_phase_rad, sizeof(float));

                    // carrier and code frequency
                    d_dump_file.write((char*)&d_carrier_doppler_hz, sizeof(float));
                    d_dump_file.write((char*)&d_code_freq_chips, sizeof(float));

                    //PLL commands
                    d_dump_file.write((char*)&carr_error_hz, sizeof(float));
                    d_dump_file.write((char*)&carr_error_filt_hz, sizeof(float));

                    //DLL commands
                    d_dump_file.write((char*)&code_error_chips, sizeof(float));
                    d_dump_file.write((char*)&code_error_filt_chips, sizeof(float));

                    // CN0 and carrier lock test
                    d_dump_file.write((char*)&d_CN0_SNV_dB_Hz, sizeof(float));
                    d_dump_file.write((char*)&d_carrier_lock_test, sizeof(float));

                    // AUX vars (for debug purposes)
                    tmp_float = d_rem_code_phase_samples;
                    d_dump_file.write((char*)&tmp_float, sizeof(float));
                    tmp_double=(double)(d_sample_counter+d_current_prn_length_samples);
                    d_dump_file.write((char*)&tmp_double, sizeof(double));


                    const gr_complex* din = (gr_complex*) input_items[0]; //PRN start block alignment
                    float in_I;
                    float in_Q;
                    for(int i = 0; i< d_current_prn_length_samples; i++)
                        {
                            in_I = (*din).real();
                            in_Q = (*din).imag();
                            d_dump_file.write((char*)&in_I, sizeof(float)); 
                            d_dump_file.write((char*)&in_Q, sizeof(float)); 
                            din++;

                        }
            }
            catch (std::ifstream::failure e)
            {
                    LOG(WARNING) << "Exception writing trk dump file " << e.what();
            }
        }

    consume_each(d_current_prn_length_samples); // this is necessary in gr::block derivates
        
    d_sample_counter += d_current_prn_length_samples; //count for the processed samples
    d_sample_counter_m += d_current_prn_length_samples_m; //count for the processed samples
    //LOG(INFO)<<"GPS tracking output end on CH="<<this->d_channel << " SAMPLE STAMP="<<d_sample_counter<<std::endl;
    return 1; //output tracking result ALWAYS even in the case of d_enable_tracking==false
}



void Gps_L1_Ca_Dll_Pll_CADLL_Tracking_cc::set_channel(unsigned int channel)
{
    d_channel = channel;
    LOG(INFO) << "Tracking Channel set to " << d_channel;
    // ############# ENABLE DATA FILE LOG #################
    if (d_dump == true)
        {
            if (d_dump_file.is_open() == false)
                {
                    try
                    {
                            d_dump_filename.append(boost::lexical_cast<std::string>(d_channel));
                            d_dump_filename.append(".dat");
                            d_dump_file.exceptions (std::ifstream::failbit | std::ifstream::badbit);
                            d_dump_file.open(d_dump_filename.c_str(), std::ios::out | std::ios::binary);
                            LOG(INFO) << "Tracking dump enabled on channel " << d_channel << " Log file: " << d_dump_filename.c_str() << std::endl;
                    }
                    catch (std::ifstream::failure e)
                    {
                            LOG(WARNING) << "channel " << d_channel << " Exception opening trk dump file " << e.what() << std::endl;
                    }
                }
        }
}



void Gps_L1_Ca_Dll_Pll_CADLL_Tracking_cc::set_channel_queue(concurrent_queue<int> *channel_internal_queue)
{
    d_channel_internal_queue = channel_internal_queue;
}


void Gps_L1_Ca_Dll_Pll_CADLL_Tracking_cc::set_gnss_synchro(Gnss_Synchro* p_gnss_synchro)
{
    d_acquisition_gnss_synchro = p_gnss_synchro;
}
