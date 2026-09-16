% Generate realistic 4-channel partial-discharge ADC stimulus for Vivado.
% Output format: one 48-bit hexadecimal word/line, {ch3,ch2,ch1,ch0},
% each channel is 12-bit offset-binary, sampled at 26 MSPS for 40 ms.

fs = 26e6;  duration_s = 40e-3;  n = round(fs * duration_s);
t = (0:n-1)' / fs;
rng(20260911, 'twister');                 % Reproducible noise and PD arrivals

% Background: 50 Hz phase reference, low-frequency interference, Gaussian noise.
base = 2048 + 90*sin(2*pi*50*t) + 18*sin(2*pi*1.2e6*t) + 7*randn(n,1);
ch = repmat(base, 1, 4);
ch = ch + repmat([0 35 -55 80], n, 1);    % Gain/offset mismatch between channels

% Four deliberately distinct PD signatures, so channel association can be
% checked directly in Vivado's m_axis_tdata[26:25] and event CSV.
% ch0: positive, large, phase-locked bursts near 45 degrees.
% ch1: negative, medium pulses near 135 degrees.
% ch2: alternating polarity, short high-rate burst near 225 degrees.
% ch3: sparse bipolar, long ring-down pulses near 315 degrees.
profiles = [ 45,  8, +1, 700, 10, 7; ...
            135, 11, -1, 520, 16, 6; ...
            225,  5,  0, 430,  7, 4; ...
            315, 17,  0, 620, 28, 11];
for c = 1:4
    phase_deg = profiles(c,1); count = profiles(c,2);
    for k = 1:count
        cycle = mod(k-1, 2);                % spread events across the two 50 Hz cycles
        center = round((cycle*20e-3 + phase_deg/360/50)*fs) + randi([-60,60]);
        width = profiles(c,5); decay = profiles(c,6);
        idx = max(1,center) + (0:width-1); idx = idx(idx <= n);
        if profiles(c,3) == 0
            polarity = 2*mod(k,2)-1;
        else
            polarity = profiles(c,3);
        end
        amp = polarity * (profiles(c,4) * (0.85 + 0.30*rand));
        shape = exp(-(0:numel(idx)-1)'/decay) .* sin(2*pi*(0:numel(idx)-1)'/4);
        ch(idx,c) = ch(idx,c) + amp*shape;
        % weak cross-channel pickup: 4--8%, never a primary event.
        for other = 1:4
            if other ~= c, ch(idx,other) = ch(idx,other) + 0.06*amp*shape; end
        end
    end
end

% Clip and convert to 12-bit offset-binary unsigned codes.
codes = uint16(max(0, min(4095, round(ch))));
words = bitor(bitor(uint64(codes(:,1)), bitshift(uint64(codes(:,2)),12)), ...
             bitor(bitshift(uint64(codes(:,3)),24), bitshift(uint64(codes(:,4)),36)));

out = 'pd_adc_4ch_26m_40ms.mem';
fid = fopen(out, 'w');
assert(fid >= 0, 'Cannot open %s for writing', out);
fprintf(fid, '%012X\n', words);
fclose(fid);

plot_n = min(n, round(200e-6*fs));
figure('Name','PD ADC stimulus');
plot(t(1:plot_n)*1e6, double(codes(1:plot_n,:))); grid on;
xlabel('Time (us)'); ylabel('ADC offset-binary code'); legend('ch0','ch1','ch2','ch3');
title('First 200 us of generated 4-channel ADC stimulus');
fprintf('Wrote %s: %d samples, %.3f ms, four 12-bit channels at %.3f MSPS.\n', ...
        out, n, duration_s*1e3, fs/1e6);
