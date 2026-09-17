# Load after launching Behavioral Simulation with tb_pd_fft_chain as top.
# The script deliberately uses top-level aliases from the testbench, so it is
# stable even if Vivado changes generated FFT/IP hierarchy names.
restart
add_wave -divider {Clock and reset}
add_wave /tb_pd_fft_chain/clk
add_wave /tb_pd_fft_chain/rst_n

add_wave -divider {Four-channel sampled tones}
add_wave -radix unsigned /tb_pd_fft_chain/filt_dv
add_wave -radix unsigned /tb_pd_fft_chain/filt_data
add_wave -radix unsigned /tb_pd_fft_chain/input_overflow

add_wave -divider {FFT configuration handshakes}
add_wave -radix hex /tb_pd_fft_chain/cfg0_d
add_wave /tb_pd_fft_chain/cfg0_v
add_wave /tb_pd_fft_chain/cfg0_r
add_wave -radix hex /tb_pd_fft_chain/cfg1_d
add_wave /tb_pd_fft_chain/cfg1_v
add_wave /tb_pd_fft_chain/cfg1_r
add_wave -radix hex /tb_pd_fft_chain/cfg2_d
add_wave /tb_pd_fft_chain/cfg2_v
add_wave /tb_pd_fft_chain/cfg2_r
add_wave -radix hex /tb_pd_fft_chain/cfg3_d
add_wave /tb_pd_fft_chain/cfg3_v
add_wave /tb_pd_fft_chain/cfg3_r

add_wave -divider {FFT CH0 output: bin and frame boundary}
add_wave -radix hex /tb_pd_fft_chain/fft0_d
add_wave -radix unsigned /tb_pd_fft_chain/fft0_u
add_wave /tb_pd_fft_chain/fft0_v
add_wave /tb_pd_fft_chain/fft0_r
add_wave /tb_pd_fft_chain/fft0_l
add_wave -divider {FFT CH1 output}
add_wave -radix hex /tb_pd_fft_chain/fft1_d
add_wave -radix unsigned /tb_pd_fft_chain/fft1_u
add_wave /tb_pd_fft_chain/fft1_v
add_wave /tb_pd_fft_chain/fft1_r
add_wave /tb_pd_fft_chain/fft1_l
add_wave -divider {FFT CH2 output}
add_wave -radix hex /tb_pd_fft_chain/fft2_d
add_wave -radix unsigned /tb_pd_fft_chain/fft2_u
add_wave /tb_pd_fft_chain/fft2_v
add_wave /tb_pd_fft_chain/fft2_r
add_wave /tb_pd_fft_chain/fft2_l
add_wave -divider {FFT CH3 output}
add_wave -radix hex /tb_pd_fft_chain/fft3_d
add_wave -radix unsigned /tb_pd_fft_chain/fft3_u
add_wave /tb_pd_fft_chain/fft3_v
add_wave /tb_pd_fft_chain/fft3_r
add_wave /tb_pd_fft_chain/fft3_l

add_wave -divider {Selected-bin monitor verdict}
add_wave -radix binary /tb_pd_fft_chain/mon_valid_sticky
add_wave -radix binary /tb_pd_fft_chain/mon_frame_seen
add_wave -radix unsigned /tb_pd_fft_chain/mon_frame_count0
add_wave -radix unsigned /tb_pd_fft_chain/mon_frame_count1
add_wave -radix unsigned /tb_pd_fft_chain/mon_frame_count2
add_wave -radix unsigned /tb_pd_fft_chain/mon_frame_count3
add_wave -radix unsigned /tb_pd_fft_chain/mon_magnitude0
add_wave -radix unsigned /tb_pd_fft_chain/mon_magnitude1
add_wave -radix unsigned /tb_pd_fft_chain/mon_magnitude2
add_wave -radix unsigned /tb_pd_fft_chain/mon_magnitude3

run all
