
#include <ctime>

#include "pulsed_jammer_detector_cc.h"

#include <sstream>

#include <boost/filesystem.hpp>

#include <gnuradio/io_signature.h>

#include <gnuradio/filter/firdes.h>

#include <glog/logging.h>

#include <volk/volk.h>

#include <volk_gnsssdr/volk_gnsssdr.h>

#include <gnuradio/fft/window.h>

#include "jammer_detector_msg.h"

#include "sdr_signal_processing.h"

using google::LogMessage;
pulsed_jammer_detector_cc_sptr pulsed_jammer_make_detector_cc(long fs_in_hz,unsigned int block_samples,unsigned int fft_samples,unsigned int noise_estimations,unsigned int A,unsigned int B,unsigned int max_dwells,bool dump,std::string dump_filename){return pulsed_jammer_detector_cc_sptr(new pulsed_jammer_detector_cc(fs_in_hz, block_samples,fft_samples, noise_estimations, A, B, max_dwells, dump,dump_filename));}pulsed_jammer_detector_cc::pulsed_jammer_detector_cc(long fs_in_hz,unsigned int block_samples,unsigned int fft_samples,unsigned int noise_estimations,unsigned int A,unsigned int B,unsigned int max_dwells,bool dump,std::string dump_filename) :gr::block("pulsed_jammer_detector_cc",gr::io_signature::make(1, 1, sizeof(gr_complex) * block_samples),gr::io_signature::make(0, 0, sizeof(gr_complex) * block_samples) ){this->message_port_register_out(pmt::mp("jammers"));d_sample_counter = 0;    d_active = false;d_state = 0;d_rf_freq = 0.0;d_fs_in = fs_in_hz;d_block_samples = block_samples;d_mean = 0.0;d_input_power = 0.0;d_jammer_power = 0.0;d_noise_power = 0.0;d_power_counter = 0;d_test_statistics = 0.0;d_threshold = 0.0;d_fft_size = fft_samples;d_k = B;d_A = A;d_B = B;d_max_dwells = max_dwells;d_dwell_counter = 0;d_noise_floor_estimated = false;d_noise_power_estimations = noise_estimations;d_magnitude_block_size = static_cast<float*>(volk_malloc(d_block_samples * sizeof(float), volk_get_alignment()));d_magnitude_fft_size = static_cast<float*>(volk_malloc(d_fft_size * sizeof(float), volk_get_alignment()));d_num_time_bins = floor((2*d_block_samples)/static_cast<double>(d_fft_size))-1;DLOG(INFO) << "Number of time bins = " << d_num_time_bins;d_magnitude_num_bins = static_cast<float*>(volk_malloc(d_num_time_bins * sizeof(float), volk_get_alignment()));d_sft_spectrogram = new gr_complex*[d_num_time_bins];for (unsigned int time_index = 0; time_index < d_num_time_bins; time_index++){d_sft_spectrogram[time_index] = static_cast<gr_complex*>(volk_malloc(d_fft_size * sizeof(gr_complex), volk_get_alignment()));}d_fft = new gr::fft::fft_real_fwd (d_num_time_bins);d_dump = dump;d_dump_filename = dump_filename;}pulsed_jammer_detector_cc::~pulsed_jammer_detector_cc(){delete d_fft;for (unsigned int i = 0; i < d_num_time_bins; i++){volk_free(d_sft_spectrogram[i]);}delete[] d_sft_spectrogram;volk_free(d_magnitude_num_bins);volk_free(d_magnitude_fft_size);volk_free(d_magnitude_block_size);if (d_dump){d_dump_file.close();}}void pulsed_jammer_detector_cc::init(){d_state=0;}void pulsed_jammer_detector_cc::set_state(int state){d_state = state;}int pulsed_jammer_detector_cc::general_work(int noutput_items,gr_vector_int &ninput_items, gr_vector_const_void_star &input_items,gr_vector_void_star &output_items __attribute__((unused))){switch (d_state){case 0:{if (d_active){d_state = 1;}d_sample_counter += d_block_samples * ninput_items[0]; DLOG(INFO) << "Pulsed jammer detector standby. Consumed " << ninput_items[0] << " items";consume_each(ninput_items[0]);break;}case 1:{uint32_t indext = 0;const gr_complex *in = (const gr_complex *)input_items[0]; DLOG(INFO) <<"Pulsed jammer detector running at samplestamp: "<< d_sample_counter << ", threshold: "<< d_threshold;float input_power = 0.0;volk_32fc_magnitude_squared_32f(d_magnitude_block_size, in, d_block_samples);volk_32f_accumulator_s32f(&input_power, d_magnitude_block_size, d_block_samples);input_power /= static_cast<float>(d_block_samples);d_input_power += input_power;d_power_counter += 1;if (d_noise_floor_estimated == true){d_dwell_counter += 1;unsigned int overlap = floor(d_fft_size/2.0);gr::filter::firdes::win_type window_type = gr::filter::firdes::WIN_KAISER;spectrogram(d_sft_spectrogram, in, d_block_samples, window_type, d_fft_size, overlap);for (unsigned int time_index = 0; time_index < d_num_time_bins; time_index++){volk_32fc_magnitude_32f(d_magnitude_fft_size, d_sft_spectrogram[time_index], d_fft_size);volk_32f_accumulator_s32f(&d_magnitude_num_bins[time_index], d_magnitude_fft_size, d_fft_size);}volk_32f_accumulator_s32f(&d_mean, d_magnitude_num_bins, d_num_time_bins);d_mean /= static_cast<float>(d_num_time_bins);for (unsigned int time_index = 0; time_index < d_num_time_bins; time_index++){if(d_magnitude_num_bins[time_index] > d_mean)  d_magnitude_num_bins[time_index] = 1.0;else d_magnitude_num_bins[time_index] = -1.0;}memcpy(d_fft->get_inbuf(), d_magnitude_num_bins, sizeof(float) * d_num_time_bins);d_fft->execute();volk_32fc_magnitude_squared_32f(d_magnitude_num_bins, d_fft->get_outbuf(), d_fft->outbuf_length());volk_gnsssdr_32f_index_max_32u(&indext, d_magnitude_num_bins, d_fft->outbuf_length());d_test_statistics = d_magnitude_num_bins[indext];if (d_test_statistics > d_threshold){d_k += 1;if (d_k == d_A){d_jammer_power = d_input_power/d_power_counter;if(10.0*log10(d_jammer_power/d_noise_power) < 0){d_state = 3; d_jammer_power = 0.0;}else d_state = 2; d_k = d_B;d_input_power = 0.0;d_power_counter = 0;d_dwell_counter = 0;}}
else
{d_k -= 1;if ((d_k == 0) || (d_dwell_counter == d_max_dwells)){d_k = d_B;d_state = 3; d_jammer_power = 0.0;d_input_power = 0.0;d_power_counter = 0;d_dwell_counter = 0;}}}else if (d_power_counter == d_noise_power_estimations){d_noise_power = d_input_power/d_power_counter;d_input_power = 0.0;d_power_counter = 0;d_noise_floor_estimated = true;DLOG(INFO) << "WB jammer detector noise estimation done. Consumed "<< ninput_items[0] <<" items.";DLOG(INFO) << "RF center freq " << d_rf_freq;DLOG(INFO) << "d_state " << d_state;std::cout << "Pulsed jammer detector noise estimation done. Noise power = "<< d_noise_power << std::endl;}d_sample_counter += d_block_samples * ninput_items[0]; consume_each(1);break;}case 2:{DLOG(INFO) << "Pulsed jammer positive detection";DLOG(INFO) << "RF center freq " << d_rf_freq;DLOG(INFO) << "sample_stamp " << d_sample_counter;DLOG(INFO) << "test statistics value " << d_test_statistics;DLOG(INFO) << "test statistics threshold " << d_threshold;d_active = false;d_state = 0;d_sample_counter += d_block_samples * ninput_items[0]; jammer.jammer_type = 1;jammer.jnr_db = 10.0*log10(d_jammer_power/d_noise_power); jammer.rf_freq_hz=d_rf_freq;jammer.test_value=d_test_statistics;jammer.sample_counter=d_sample_counter;jammer.timestamp= time(0);jammer.detection=true;std::shared_ptr<jammer_detector_msg> tmp_obj = std::make_shared<jammer_detector_msg>(jammer);this->message_port_pub(pmt::mp("jammers"), pmt::make_any(tmp_obj));DLOG(INFO) << "Pulsed jammer detector positive message sended. Consumed " << ninput_items[0] << " items.";consume_each(ninput_items[0]);break;}case 3:{DLOG(INFO) << "Pulsed jammer negative detection";DLOG(INFO) << "RF center freq " << d_rf_freq;DLOG(INFO) << "sample_stamp " << d_sample_counter;DLOG(INFO) << "test statistics value " << d_test_statistics;DLOG(INFO) << "test statistics threshold " << d_threshold;d_active = false;d_state = 0;d_sample_counter += d_block_samples * ninput_items[0]; jammer.jammer_type=1;jammer.jnr_db = 10.0*log10(d_jammer_power/d_noise_power); jammer.rf_freq_hz=d_rf_freq;jammer.test_value=d_test_statistics;jammer.sample_counter=d_sample_counter;jammer.timestamp= time(0);jammer.detection=false;std::shared_ptr<jammer_detector_msg> tmp_obj = std::make_shared<jammer_detector_msg>(jammer);this->message_port_pub(pmt::mp("jammers"), pmt::make_any(tmp_obj));DLOG(INFO) << "Pulsed jammer detector negative message sended. Consumed " << ninput_items[0] << " items.";consume_each(ninput_items[0]);break;}}return noutput_items;}