% Generate a reproducible, physics-inspired 4-channel PD waveform for simulation.
%
% IMPORTANT: this is a synthetic test stimulus, not a field measurement.
% It models a narrow fast front followed by damped sensor ringing, channel
% coupling, 50 Hz phase clusters, baseline interference, and band-limited noise.
%
% Output is compatible with tb_pd_feature_dds.v:
%   pd_adc_4ch_65m_40ms.mem       2,600,000 lines of 48-bit {CH3,CH2,CH1,CH0}
%   pd_event_truth_physical_65m.csv expected event sample/phase/amplitude table
%
% Run from MATLAB with:
%   run('F:/xinya/v5/merge/pd_feature_bd_2020_2_dds_onboard_iir_ddr1g_20260916/sim/generate_pd_adc_waveform_physical_model.m')

clearvars;
close all;
clc;

%% Acquisition and reproducibility parameters
fsHz = 65e6;
mainsHz = 50;
cycleCount = 2;
samplesPerCycle = round(fsHz / mainsHz);  % 1,300,000 at 65 MSPS / 50 Hz
assert(samplesPerCycle*mainsHz == fsHz, ...
       'Sample rate must be an integer multiple of the mains frequency.');
sampleCount = cycleCount * samplesPerCycle;
t = single((0:sampleCount-1)' / fsHz);
rng(20261008, 'twister');                 % repeatable stimulus and event truth

adcMid = 2048;
adcMax = 4095;
phaseCenterDeg = [45, 135, 225, 315];
phaseStdDeg = [4, 5, 6, 7];
eventsPerCycle = [8, 9, 7, 10];
nominalAmpCode = [700, 520, 450, 600];
ringCenterHz = [1.25e6, 0.95e6, 1.55e6, 1.10e6];
channelGain = single([1.00, 0.88, 0.76, 0.94]);
channelOffset = single([-8, 10, 16, -20]);

%% Analog-equivalent baseline, interference, and noise in ADC-code units
mainsPickup = single(24) * sin(single(2*pi*mainsHz)*t);
emi = single(7) * sin(single(2*pi*1.20e6)*t + single(0.37)) + ...
      single(3) * sin(single(2*pi*3.40e6)*t - single(0.21));
baseline = single(adcMid) + repmat(channelOffset, sampleCount, 1) + ...
           (mainsPickup + emi) * channelGain;

% Common-mode and per-channel colored noise; all values remain in raw ADC LSB.
rhoCommon = single(exp(-2*pi*1.0e6/fsHz));
rhoChannel = single(exp(-2*pi*1.7e6/fsHz));
commonNoise = single(1.8) * filter(sqrt(single(1)-rhoCommon^2), ...
                           single([1, -rhoCommon]), ...
                           randn(sampleCount, 1, 'single'));
noise = repmat(commonNoise, 1, 4);
for c = 1:4
    colored = filter(sqrt(single(1)-rhoChannel^2), ...
                     single([1, -rhoChannel]), ...
                     randn(sampleCount, 1, 'single'));
    noise(:, c) = noise(:, c) + single(1.4)*colored + ...
                  single(0.65)*randn(sampleCount, 1, 'single');
end

%% Generate distinct channel event clusters over every full 50 Hz cycle
primaryEvents = zeros(sampleCount, 4, 'single');
totalEvents = cycleCount * sum(eventsPerCycle);
eventChannel = zeros(totalEvents, 1);
eventCycle = zeros(totalEvents, 1);
eventSample0 = zeros(totalEvents, 1);
eventPhaseDeg = zeros(totalEvents, 1);
eventPolarity = zeros(totalEvents, 1);
eventAmplitudeCode = zeros(totalEvents, 1);
eventRingHz = zeros(totalEvents, 1);
eventNumber = 0;

for c = 1:4
    for cyc = 0:cycleCount-1
        eventPhases = phaseCenterDeg(c) + ...
                      phaseStdDeg(c)*randn(eventsPerCycle(c), 1);
        eventPhases = min(max(eventPhases, 2), 358);

        for k = 1:eventsPerCycle(c)
            eventNumber = eventNumber + 1;
            phase = eventPhases(k);
            sampleJitter = randi([-3, 3]);
            centerSample = round((cyc + phase/360) * samplesPerCycle) + ...
                           1 + sampleJitter;

            % Fast-rise / exponential-tail front plus a damped resonant sensor
            % response. Time constants and resonance vary slightly per event.
            tauRise = (35 + 20*rand) * 1e-9;
            tauFall = (0.55 + 0.55*rand) * 1e-6;
            tauRing = (0.85 + 0.90*rand) * 1e-6;
            fRing = ringCenterHz(c) * (0.88 + 0.24*rand);
            pulseLength = min(1024, ceil(8*tauRing*fsHz));
            u = (0:pulseLength-1)' / fsHz;

            fastFront = exp(-u/tauFall) - exp(-u/tauRise);
            fastFront = fastFront / max(abs(fastFront));
            ring = exp(-u/tauRing) .* sin(2*pi*fRing*u);
            ring = ring / max(abs(ring));
            pulse = 0.68*fastFront + 0.32*ring;
            pulse = pulse / max(abs(pulse));

            if c == 1
                polarity = 1;
            elseif c == 2
                polarity = -1;
            elseif c == 3
                polarity = 1 - 2*mod(eventNumber, 2); % alternating polarity
            else
                polarity = 2*randi([0, 1]) - 1;       % sparse bipolar channel
            end
            amplitude = round(nominalAmpCode(c) * (0.78 + 0.44*rand));

            dst = centerSample:min(sampleCount, centerSample+pulseLength-1);
            src = 1:numel(dst);
            primaryEvents(dst, c) = primaryEvents(dst, c) + ...
                                    single(polarity * amplitude * pulse(src));

            actualSample0 = centerSample - 1;
            eventChannel(eventNumber) = c - 1;
            eventCycle(eventNumber) = cyc;
            eventSample0(eventNumber) = actualSample0;
            eventPhaseDeg(eventNumber) = mod(actualSample0/samplesPerCycle*360, 360);
            eventPolarity(eventNumber) = polarity;
            eventAmplitudeCode(eventNumber) = amplitude;
            eventRingHz(eventNumber) = fRing;
        end
    end
end

% Small sensor/electromagnetic pickup from a primary channel into other inputs.
% Rows are the source channel; columns are the receiving channel.
coupling = single([1.000, 0.035, 0.020, 0.015; ...
            0.040, 1.000, 0.025, 0.020; ...
            0.020, 0.030, 1.000, 0.040; ...
            0.015, 0.020, 0.035, 1.000]);
analog = baseline + noise + primaryEvents*coupling;

%% Quantize to the project's 12-bit offset-binary ADC format
clipMask = analog < single(0) | analog > single(adcMax);
clipCount = nnz(clipMask);
codes = uint16(min(single(adcMax), max(single(0), round(analog))));

% Project packing, matching $readmemh into reg [47:0]:
% {CH3[11:0], CH2[11:0], CH1[11:0], CH0[11:0]}.
packed = bitor(bitshift(uint64(codes(:,4)), 36), ...
        bitor(bitshift(uint64(codes(:,3)), 24), ...
        bitor(bitshift(uint64(codes(:,2)), 12), uint64(codes(:,1)))));

scriptDir = fileparts(mfilename('fullpath'));
memPath = fullfile(scriptDir, 'pd_adc_4ch_65m_40ms.mem');
truthPath = fullfile(scriptDir, 'pd_event_truth_physical_65m.csv');

fid = fopen(memPath, 'w');
assert(fid >= 0, 'Cannot open output file: %s', memPath);
fileCleanup = onCleanup(@() fclose(fid));
written = fprintf(fid, '%012X\n', packed);
assert(written > 0, 'Failed while writing stimulus file: %s', memPath);
clear fileCleanup;

truth = table(eventChannel, eventCycle, eventSample0, eventPhaseDeg, ...
              eventPolarity, eventAmplitudeCode, eventRingHz, ...
    'VariableNames', {'channel0to3', 'cycle0based', 'sample0based', ...
                      'phaseDeg', 'polarity', 'amplitudeCode', 'ringHz'});
writetable(truth, truthPath);

%% Per-channel plots centered on each channel's first injected pulse
figure('Name', 'Synthetic PD waveform - first event per channel', ...
       'Color', 'w');
layout = tiledlayout(4, 1, 'TileSpacing', 'compact');
colors = [0.90, 0.62, 0.00; 0.00, 0.55, 0.52; ...
          0.88, 0.30, 0.25; 0.65, 0.20, 0.75];
for c = 1:4
    firstEvent = find(eventChannel == c-1, 1, 'first');
    center = eventSample0(firstEvent) + 1;
    lo = max(1, center-512);
    hi = min(sampleCount, center+1023);
    sampleIndex = (lo:hi)';
    nexttile;
    plot((sampleIndex-center)/fsHz*1e6, ...
         double(codes(sampleIndex,c))-adcMid, ...
         'Color', colors(c,:), 'LineWidth', 0.8);
    grid on;
    ylabel(sprintf('CH%d (LSB)', c-1));
    title(sprintf('CH%d first injected PD pulse, phase %.2f deg', ...
          c-1, eventPhaseDeg(firstEvent)));
end
xlabel(layout, 'Time from pulse center (us)');

fprintf('Generated synthetic stimulus (not field-measured data):\n');
fprintf('  Samples: %d = %d cycles x %d samples/cycle at %.3f MSPS\n', ...
        sampleCount, cycleCount, samplesPerCycle, fsHz/1e6);
fprintf('  Injected primary events: %d; clipped ADC samples: %d\n', ...
        totalEvents, clipCount);
fprintf('  MEM:   %s\n', memPath);
fprintf('  Truth: %s\n', truthPath);
