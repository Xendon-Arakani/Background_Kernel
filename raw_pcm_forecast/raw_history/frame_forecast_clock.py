
from dataclasses import dataclass
import copy
import math
from pathlib import Path
import torch
from history_model import HistoryForecaster
from torch_cuda_runtime import BundledCuda
from history_decode import decode_origins


@dataclass
class FrameForecastBatch:
    first_anchor: int
    model_version: int
    trained_until: int
    points: torch.Tensor
    ready: torch.cuda.Event

    @property
    def rows(self):
        return self.points.shape[0]


@dataclass
class InferenceState:
    model: HistoryForecaster
    version: int
    trained_until: int
    ready: torch.cuda.Event

    @classmethod
    @torch.no_grad()
    def capture_infer_state(cls, training_model, version, *, trained_until):
        if trained_until < 0:
            raise ValueError("The training source boundary cannot be negative")


        model = copy.deepcopy(training_model).cuda().eval().requires_grad_(False)
        event = torch.cuda.Event(blocking=True)
        event.record()
        return cls(model, int(version), int(trained_until), event)


class FrameForecastClock:
    def __init__(self, model: HistoryForecaster, history: torch.Tensor, anchor: int, version=0,
                 prepared_module=None):
        if not history.is_cuda or history.shape != (model.history, 2) or anchor < model.history:
            raise ValueError("A complete raw stereo history and source anchor are required")
        if history.dtype != torch.float32:
            raise ValueError("PCM stays float32")
        self.stream = torch.cuda.current_stream()
        self.device = self.stream.device
        if history.device != self.device:
            raise ValueError("StereoHistory must be on the inference owner's CUDA device")



        snapshot = InferenceState.capture_infer_state(model, version, trained_until=0)
        torch.cuda.current_stream().wait_event(snapshot.ready)
        self.model = snapshot.model
        self.module = prepared_module or BundledCuda(Path(__file__).with_name("frame_forecast_cuda.cu"))
        self.anchor, self.version = int(anchor), int(version)
        self.trained_until = 0
        self.history = history.clone().contiguous()
        self.position = 0
        torch.backends.cuda.matmul.allow_tf32 = False
        self._rebuild()


        cache = torch.cuda.get_device_properties(self.device).L2_cache_size
        per_row = model.horizon * (16 + 2) * 4
        self.tile_rows = max(1, (cache - self.basis.numel() * 4) // per_row)
        self.rows_emitted = 0

    @classmethod
    @torch.no_grad()
    def warm_cuda_forecast(cls, model):

        scratch = cls(model, torch.zeros((model.history, 2), device="cuda"), model.history)



        cache=torch.cuda.get_device_properties(scratch.device).L2_cache_size
        direct_rows=max(1, (math.isqrt((model.horizon-1)**2+4*(cache//8))-(model.horizon-1))//2)
        warm_rows=min(scratch.tile_rows,direct_rows)
        warm = scratch.forecast_packet(torch.zeros((warm_rows, 2), device="cuda"), model.history)
        warm.ready.synchronize()
        return scratch.module

    def _check_owner(self):
        if torch.cuda.current_stream() != self.stream:
            raise RuntimeError("One inference owner/stream; hand off weights with InferenceState")

    @torch.no_grad()
    def _rebuild(self):
        model = self.model



        for parameter in model.parameters():
            parameter.record_stream(self.stream)
        self.state = torch.view_as_real(model.encode_raw_history(self.copy_raw_history())).reshape(64).contiguous()
        pole = model.continuous_poles()
        self.step = torch.view_as_real(torch.exp(pole / model.rate)).reshape(64).contiguous()
        self.eviction = torch.view_as_real(torch.exp(pole * (model.history / model.rate))).reshape(64).contiguous()
        time = torch.arange(model.horizon, device="cuda", dtype=torch.float64) / model.rate
        basis = torch.view_as_real(torch.exp(pole[:, None] * time[None, :]))
        self.basis = basis.permute(0, 2, 1).reshape(64, model.horizon).float().contiguous()
        self.direct_kernel=(model.direct_weight/math.sqrt(model.history)).contiguous()

    def publish_new_weights(self, snapshot: InferenceState):

        self._check_owner()
        if snapshot.version <= self.version:
            raise ValueError("Weight versions must strictly advance")
        if snapshot.trained_until > self.anchor:
            raise ValueError("Training labels are ahead of inference; keep the previous snapshot")
        if snapshot.trained_until < self.trained_until:
            raise ValueError("Training source boundaries cannot move backward")
        old, new = self.model, snapshot.model
        if (new.history, new.horizon, new.rate) != (old.history, old.horizon, old.rate):
            raise ValueError("Weight snapshot changed the forecast contract")
        torch.cuda.current_stream().wait_event(snapshot.ready)
        self.model = new
        self.version = snapshot.version
        self.trained_until = snapshot.trained_until
        self._rebuild()

    def copy_raw_history(self):
        self._check_owner()
        return torch.cat((self.history[self.position:], self.history[:self.position])).contiguous()

    @torch.no_grad()
    def forecast_packet(self, incoming, first_frame):
        self._check_owner()
        if first_frame != self.anchor:
            raise ValueError("Source gap/overlap: no prediction rows may be skipped")
        if incoming.ndim != 2 or incoming.shape[1] != 2 or not len(incoming):
            raise ValueError("Expected nonempty stereo packet")
        if not incoming.is_cuda or incoming.dtype != torch.float32:
            raise ValueError("Incoming PCM must be float32 on the GPU")
        if incoming.device != self.device:
            raise ValueError("Incoming PCM must be on the inference owner's CUDA device")
        incoming = incoming.contiguous()
        incoming.record_stream(self.stream)
        rows = len(incoming)



        raw_pcm=torch.cat((self.copy_raw_history(),incoming))[1:]
        columns=rows+self.model.horizon-1
        base=decode_origins(raw_pcm,self.model.direct_weight,self.model.history,
                             1,0,columns)[0]
        raw_points=torch.empty((rows,self.model.horizon,2),device=incoming.device)
        self.module.launch("decode_complete_raw_history_for_each_source_origin",((columns+255)//256,1,1),(256,1,1),
                           [raw_pcm,self.direct_kernel,base,raw_points,
                            self.model.history,self.model.horizon,rows])


        if rows <= self.model.history:
            count = min(rows, self.model.history - self.position)
            expired = (self.history[self.position:self.position + rows] if count == rows else
                       torch.cat((self.history[self.position:], self.history[:rows - count])))
        else:
            expired = torch.cat((self.copy_raw_history(), incoming[:-self.model.history]))
        expired = expired.contiguous()
        states = torch.empty((rows, 64), device="cuda", dtype=torch.float64)
        self.module.launch("advance_compact_context_for_each_source_frame", (1, 1, 1), (32, 1, 1),
                           [self.state, incoming, expired, self.step, self.eviction,
                            self.model.input_weight, states, rows])
        factored_rows = torch.empty((rows * 16, 64), device="cuda", dtype=torch.float32)
        self.module.launch("factor_compact_context_decoder_by_origin", ((rows * 16 * 32 + 255) // 256, 1, 1), (256, 1, 1),
                           [states, self.model.hidden.weight, factored_rows, rows])
        output = torch.empty((rows, self.model.horizon, 2), device="cuda", dtype=torch.float32)
        workspace = torch.empty((min(rows, self.tile_rows) * 16, self.model.horizon),
                                device="cuda", dtype=torch.float32)
        for start in range(0, rows, self.tile_rows):
            count = min(self.tile_rows, rows - start)
            hidden = workspace[:count * 16]
            torch.mm(factored_rows[start * 16:(start + count) * 16], self.basis, out=hidden)
            self.module.launch("materialize_all_future_leads_for_each_source_origin", ((self.model.horizon + 255) // 256, count, 1),
                               (256, 1, 1), [hidden, self.model.hidden.bias,
                                            self.model.output.weight, self.model.output.bias,
                                            raw_points[start:start+count],
                                            output[start:start + count], count, self.model.horizon])
        self.state = states[-1].clone()
        if rows >= self.model.history:
            self.history.copy_(incoming[-self.model.history:])
            self.position = 0
        else:
            count = min(rows, self.model.history - self.position)
            self.history[self.position:self.position + count].copy_(incoming[:count])
            self.history[:rows - count].copy_(incoming[count:])
            self.position = (self.position + rows) % self.model.history
        result = FrameForecastBatch(self.anchor + 1, self.version, self.trained_until, output,
                               torch.cuda.Event(blocking=True))
        result.ready.record()
        self.anchor += rows
        self.rows_emitted += rows
        return result
