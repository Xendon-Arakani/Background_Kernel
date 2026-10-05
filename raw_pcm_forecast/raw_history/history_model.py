
from __future__ import annotations

import math
import torch
from torch import nn
from window_encode import encode_raw_tiles
from history_decode import decode_one_future

RATE = 48_000
HISTORY = 480_000
HORIZON = 48_000
MODES = 32
CHECKPOINT_SCHEMA = 3
ARCHITECTURE = "full-raw-pcm-fir-plus-64-context-tanh-output"


class HistoryForecaster(nn.Module):
    checkpoint_schema=CHECKPOINT_SCHEMA
    architecture=ARCHITECTURE
    def __init__(self, history=HISTORY, horizon=HORIZON, rate=RATE, seed=1):
        super().__init__()
        self.history, self.horizon, self.rate = history, horizon, rate
        generator = torch.Generator().manual_seed(seed)

        frequencies = torch.logspace(math.log10(rate / history),
                                     math.log10(rate / 2), MODES, dtype=torch.float64)
        decay = torch.logspace(math.log10(rate / history),
                               math.log10(rate / horizon), MODES, dtype=torch.float64)
        self.frequency_hz = nn.Parameter(frequencies)
        self.log_decay = nn.Parameter(decay.log())
        self.input_weight = nn.Parameter(
            torch.randn(MODES, 2, 2, generator=generator, dtype=torch.float64) / math.sqrt(history))
        self.hidden = nn.Linear(64, 16)

        self.output = nn.Linear(16, 2)


        self.sample_height = nn.Tanh()


        self.direct_weight=nn.Parameter(torch.zeros(2,history+horizon-1))
        with torch.no_grad():
            self.hidden.weight.copy_(torch.randn(16, 64, generator=generator) / math.sqrt(64))
            self.hidden.bias.zero_()
            self.output.weight.copy_(torch.randn(2, 16, generator=generator) / math.sqrt(16))
            self.output.bias.zero_()


            self.direct_weight[:,0]=math.sqrt(history)

    def continuous_poles(self):
        return torch.complex(-self.log_decay.exp(), 2 * math.pi * self.frequency_hz)

    def stereo_weights(self):

        return torch.view_as_complex(self.input_weight.contiguous())

    def encode_raw_history(self, x):
        if x.shape != (self.history, 2):
            raise ValueError(f"Expected exactly {self.history} stereo frames")

        return encode_raw_tiles(x, self.frequency_hz, self.log_decay,
                            self.input_weight, self.rate, self.horizon)

    def decode_horizon(self, state, raw_history):
        if state.shape != (MODES,):
            raise ValueError("Expected 32 complex (64 real) state coordinates")
        if raw_history.shape != (self.history,2):
            raise ValueError("The decoder must receive the full signed raw history")
        time = torch.arange(self.horizon, device=state.device, dtype=torch.float64) / self.rate
        future = torch.exp(time[:, None] * self.continuous_poles()[None, :]) * state[None, :]
        features = torch.view_as_real(future).reshape(self.horizon, 64)
        features = features.to(self.hidden.weight.dtype)
        return self.sample_height(self.output(torch.tanh(self.hidden(features)))+
                                  decode_one_future(raw_history,self.direct_weight))

    def forward(self, x):
        return self.decode_horizon(self.encode_raw_history(x),x)

    def advance_window(self, state, incoming, expired):

        step = torch.exp(self.continuous_poles() / self.rate)
        b = self.stereo_weights()
        return step * state + b @ incoming.to(torch.complex128) - (
            step ** self.history) * (b @ expired.to(torch.complex128))


def stereo_raw_mse(prediction, actual):
    if prediction.shape != actual.shape or prediction.ndim != 2 or prediction.shape[1] != 2:
        raise ValueError("MSE requires matching signed stereo samples")
    return (prediction - actual).square().mean()


def stereo_metrics(prediction, actual):
    p, y = prediction.detach().double(), actual.detach().double()
    mse = float((p - y).square().mean())
    silence = float(y.square().mean())
    return dict(mse=mse, silence_mse=silence,
                skill_vs_silence=None if silence == 0 else 1 - mse / silence,
                target_rms=math.sqrt(silence), predicted_rms=float(p.square().mean().sqrt()),
                target_peak=float(y.abs().max()), predicted_peak=float(p.abs().max()))
