#include "stdafx.h"
#include "Beatmap.hpp"
#include "KShootMap.hpp"

// Temporary object to keep track if a button is a hold button
struct TempButtonState
{
	TempButtonState(MapTime startTime)
		: startTime(startTime)
	{
	}
	MapTime startTime;
	uint32 numTicks = 0;
	EffectType effectType = EffectType::None;
	uint16 effectParams[2] = { 0 };
	// If using the smalles grid to indicate hold note duration
	bool fineSnap = false;
	// Set for hold continuations, this is where there is a hold right after an existing one but with different effects
	HoldObjectState* lastHoldObject = nullptr;

	uint8 sampleIndex = 0xFF;
	bool usingSample = false;

};
struct TempLaserState
{
	TempLaserState(MapTime startTime, uint32 effectType, TimingPoint* tpStart)
		: startTime(startTime), effectType(effectType), tpStart(tpStart)
	{
	}
	// Timing point at which this segment started
	TimingPoint* tpStart;
	MapTime startTime;
	uint32 numTicks = 0;
	uint32 effectType = 0;
	char spinType = 0;
	uint32 spinDuration = 0;
	uint8 effectParams = 0;
	float startPosition; // Entry position
	// Previous segment
	LaserObjectState* last = nullptr;
};

class EffectTypeMap
{
	// Custom effect types (1.60)
	uint16 m_customEffectTypeID = (uint16)EffectType::UserDefined0;
public:
	EffectTypeMap()
	{
		// Add common effect types
		effectTypes["None"] = EffectType::None;
		effectTypes["Retrigger"] = EffectType::Retrigger;
		effectTypes["Flanger"] = EffectType::Flanger;
		effectTypes["Phaser"] = EffectType::Phaser;
		effectTypes["Gate"] = EffectType::Gate;
		effectTypes["TapeStop"] = EffectType::TapeStop;
		effectTypes["BitCrusher"] = EffectType::Bitcrush;
		effectTypes["Wobble"] = EffectType::Wobble;
		effectTypes["SideChain"] = EffectType::SideChain;
		effectTypes["Echo"] = EffectType::Echo;
		effectTypes["Panning"] = EffectType::Panning;
		effectTypes["PitchShift"] = EffectType::PitchShift;
		effectTypes["LPF"] = EffectType::LowPassFilter;
		effectTypes["HPF"] = EffectType::HighPassFilter;
		effectTypes["PEAK"] = EffectType::PeakingFilter;
	}

	// Only checks if a mapping exists and returns this, or None
	const EffectType* FindEffectType(const String& name) const
	{
		return effectTypes.Find(name);
	}

	// Adds or returns the enum value mapping to this effect
	EffectType FindOrAddEffectType(const String& name)
	{
		EffectType* id = effectTypes.Find(name);
		if (!id)
			return effectTypes.Add(name, (EffectType)m_customEffectTypeID++);
		return *id;
	};

	Map<String, EffectType> effectTypes;
};

template<typename T>
void AssignAudioEffectParameter(EffectParam<T>& param, const String& paramName, Map<String, float>& floatParams, Map<String, int>& intParams)
{
	float* fval = floatParams.Find(paramName);
	if (fval)
	{
		param = *fval;
		return;
	}
	int32* ival = intParams.Find(paramName);
	if (ival)
	{
		param = *ival;
		return;
	}
}

struct MultiParam
{
	enum Type
	{
		Float,
		Samples,
		Int,
	};
	Type type;
	union
	{
		float fval;
		int32 ival;
	};
};
struct MultiParamRange
{
	MultiParamRange() = default;
	MultiParamRange(const MultiParam& a)
	{
		params[0] = a;
	}
	MultiParamRange(const MultiParam& a, const MultiParam& b)
	{
		params[0] = a;
		params[1] = b;
		isRange = true;
	}
	EffectParam<float> ToFloatParam()
	{
		auto r = params[0].type == MultiParam::Float ?
			EffectParam<float>(params[0].fval, params[1].fval) :
			EffectParam<float>((float)params[0].ival, (float)params[1].ival);
		r.isRange = isRange;
		return r;
	}
	EffectParam<EffectDuration> ToDurationParam()
	{
		auto r = params[0].type == MultiParam::Float ?
			EffectParam<EffectDuration>(params[0].fval, params[1].fval) :
			EffectParam<EffectDuration>(params[0].ival, params[1].ival);
		r.isRange = isRange;
		return r;
	}
	EffectParam<int32> ToSamplesParam()
	{
		EffectParam<int32> r;
		if (params[0].type == MultiParam::Int)
			r = EffectParam<int32>(params[0].ival, params[1].ival);
		r.isRange = isRange;
		return r;
	}
	MultiParam params[2];
	bool isRange = false;
};
static MultiParam ParseParam(const String& in)
{
	MultiParam ret;
	if (in.find('.') != -1)
	{
		ret.type = MultiParam::Float;
		sscanf(*in, "%f", &ret.fval);
	}
	else if (in.find('/') != -1)
	{
		ret.type = MultiParam::Float;
		String a, b;
		in.Split("/", &a, &b);
		ret.fval = (float)(atof(*a) / atof(*b));
	}
	else if (in.find("samples") != -1)
	{
		ret.type = MultiParam::Samples;
		sscanf(*in, "%i", &ret.ival);
	}
	else
	{
		ret.type = MultiParam::Int;
		sscanf(*in, "%i", &ret.ival);
	}
	return ret;
}
AudioEffect ParseCustomEffect(const KShootEffectDefinition& def)
{
	static EffectTypeMap defaultEffects;
	AudioEffect effect;
	bool typeSet = false;

	Map<String, MultiParamRange> params;
	for (auto s : def.parameters)
	{
		// This one is easy
		if (s.first == "type")
		{
			// Get the default effect for this name
			const EffectType* type = defaultEffects.FindEffectType(s.second);
			if (!type)
			{
				Logf("Unknown base effect type for custom effect type: %s", Logger::Warning, s.second);
				continue;
			}
			effect = AudioEffect::GetDefault(*type);
			typeSet = true;
		}
		else
		{
			size_t split = s.second.find('-', 1);
			if (split != -1)
			{
				String a, b;
				a = s.second.substr(0, split);
				b = s.second.substr(split + 1);

				MultiParamRange pr = { ParseParam(a), ParseParam(b) };
				if (pr.params[0].type != pr.params[1].type)
				{
					Logf("Non matching parameters types \"[%s, %s]\" for key: %s", Logger::Warning, s.first, s.second, s.first);
					continue;
				}
				params.Add(s.first, pr);
			}
			else
			{
				params.Add(s.first, ParseParam(s.second));
			}
		}
	}

	if (!typeSet)
	{
		Logf("Type not set for custom effect type: %s", Logger::Warning, def.typeName);
		return effect;
	}

	auto AssignFloatIfSet = [&](EffectParam<float>& target, const String& name)
	{
		auto* param = params.Find(name);
		if (param)
		{
			target = param->ToFloatParam();
		}
	};
	auto AssignDurationIfSet = [&](EffectParam<EffectDuration>& target, const String& name)
	{
		auto* param = params.Find(name);
		if (param)
		{
			target = param->ToDurationParam();
		}
	};
	auto AssignSamplesIfSet = [&](EffectParam<int32>& target, const String& name)
	{
		auto* param = params.Find(name);
		if (param && param->params[0].type == MultiParam::Samples)
		{
			target = param->ToSamplesParam();
		}
	};

	AssignFloatIfSet(effect.mix, "mix");

	// Set individual parameters per effect based on if they are specified or not
	// if they are not set the defaults will be kept (as aquired above)
	switch (effect.type)
	{
	case EffectType::PitchShift:
		AssignFloatIfSet(effect.pitchshift.amount, "pitch");
		break;
	case EffectType::Bitcrush:
		AssignSamplesIfSet(effect.bitcrusher.reduction, "amount");
		break;
	case EffectType::Echo:
		AssignDurationIfSet(effect.duration, "waveLength");
		AssignFloatIfSet(effect.echo.feedback, "feedbackLevel");
		break;
	case EffectType::Flanger:
		AssignDurationIfSet(effect.duration, "period");
		break;
	case EffectType::Gate:
		AssignDurationIfSet(effect.duration, "waveLength");
		AssignFloatIfSet(effect.gate.gate, "rate");
		break;
	case EffectType::Retrigger:
		AssignDurationIfSet(effect.duration, "waveLength");
		AssignFloatIfSet(effect.retrigger.gate, "rate");
		AssignDurationIfSet(effect.retrigger.reset, "updatePeriod");
		break;
	case EffectType::Wobble:
		AssignDurationIfSet(effect.duration, "waveLength");
		AssignFloatIfSet(effect.wobble.min, "loFreq");
		AssignFloatIfSet(effect.wobble.max, "hiFreq");
		AssignFloatIfSet(effect.wobble.q, "Q");
		break;
	case EffectType::TapeStop:
		AssignDurationIfSet(effect.duration, "speed");
		break;
	}

	return effect;
};

bool Beatmap::m_ProcessKShootMap(BinaryStream& input, bool metadataOnly)
{
	KShootMap kshootMap;
	if (!kshootMap.Init(input, metadataOnly))
		return false;

	EffectTypeMap effectTypeMap;
	EffectTypeMap filterTypeMap;
	Map<EffectType, int16> defaultEffectParams;

	// Set defaults
	{
		defaultEffectParams[EffectType::Bitcrush] = 4;
		defaultEffectParams[EffectType::Gate] = 8;
		defaultEffectParams[EffectType::Retrigger] = 8;
		defaultEffectParams[EffectType::Phaser] = 2000;
		defaultEffectParams[EffectType::Flanger] = 2000;
		defaultEffectParams[EffectType::Wobble] = 12;
		defaultEffectParams[EffectType::SideChain] = 8;
		defaultEffectParams[EffectType::TapeStop] = 50;
	}

	// Add all the custom effect types
	for (auto it = kshootMap.fxDefines.begin(); it != kshootMap.fxDefines.end(); it++)
	{
		EffectType type = effectTypeMap.FindOrAddEffectType(it->first);
		if (m_customEffects.Contains(type))
			continue;
		m_customEffects.Add(type, ParseCustomEffect(it->second));
	}
	for (auto it = kshootMap.filterDefines.begin(); it != kshootMap.filterDefines.end(); it++)
	{
		EffectType type = filterTypeMap.FindOrAddEffectType(it->first);
		if (m_customFilters.Contains(type))
			continue;
		m_customFilters.Add(type, ParseCustomEffect(it->second));
	}

	auto ParseFilterType = [&](const String& str)
	{
		EffectType type = EffectType::None;
		if (str == "hpf1")
		{
			type = EffectType::HighPassFilter;
		}
		else if (str == "lpf1")
		{
			type = EffectType::LowPassFilter;
		}
		else if (str == "fx;bitc" || str == "bitc")
		{
			type = EffectType::Bitcrush;
		}
		else if (str == "peak")
		{
			type = EffectType::PeakingFilter;
		}
		else
		{
			const EffectType* foundType = filterTypeMap.FindEffectType(str);
			if (foundType)
				type = *foundType;
			else
				Logf("[KSH]Unknown filter type: %s", Logger::Warning, str);
		}
		return type;
	};

	// Process map settings
	m_settings.previewOffset = 0;
	m_settings.previewDuration = 0;
	for (auto& s : kshootMap.settings)
	{
		if (s.first == "title")
			m_settings.title = s.second;
		else if (s.first == "artist")
			m_settings.artist = s.second;
		else if (s.first == "effect")
			m_settings.effector = s.second;
		else if (s.first == "illustrator")
			m_settings.illustrator = s.second;
		else if (s.first == "t")
			m_settings.bpm = s.second;
		else if (s.first == "jacket")
			m_settings.jacketPath = s.second;
		else if (s.first == "bg")
			m_settings.backgroundPath = s.second;
		else if (s.first == "layer")
			m_settings.foregroundPath = s.second;
		else if (s.first == "m")
		{
			if (s.second.find(';') != -1)
			{
				String audioFX, audioNoFX;
				s.second.Split(";", &audioNoFX, &audioFX);
				size_t splitMore = audioFX.find(';');
				if (splitMore != -1)
					audioFX = audioFX.substr(0, splitMore);
				m_settings.audioFX = audioFX;
				m_settings.audioNoFX = audioNoFX;
			}
			else
			{
				m_settings.audioNoFX = s.second;
			}
		}
		else if (s.first == "o")
		{
			m_settings.offset = atol(*s.second);
		}
		// TODO: Move initial laser effect settings to an event instead
		else if (s.first == "filtertype")
		{
			m_settings.laserEffectType = ParseFilterType(s.second);
		}
		else if (s.first == "pfiltergain")
		{
			m_settings.laserEffectMix = (float)atol(*s.second) / 100.0f;
		}
		else if (s.first == "chokkakuvol")
		{
			m_settings.slamVolume = (float)atol(*s.second) / 100.0f;
		}
		// end TODO
		else if (s.first == "level")
		{
			m_settings.level = atoi(*s.second);
		}
		else if (s.first == "difficulty")
		{
			m_settings.difficulty = 0;
			if (s.second == "challenge")
			{
				m_settings.difficulty = 1;
			}
			else if (s.second == "extended")
			{
				m_settings.difficulty = 2;
			}
			else if (s.second == "infinite")
			{
				m_settings.difficulty = 3;
			}
		}
		else if (s.first == "po")
		{
			m_settings.previewOffset = atoi(*s.second);
		}
		else if (s.first == "plength")
		{
			m_settings.previewDuration = atoi(*s.second);
		}
		else if (s.first == "total")
		{
			m_settings.total = atoi(*s.second);
		}
	}

	// Temporary map for timing points
	Map<MapTime, TimingPoint*> timingPointMap;

	// Process initial timing point
	TimingPoint* lastTimingPoint = new TimingPoint();
	lastTimingPoint->time = atol(*kshootMap.settings["o"]);
	double bpm = atof(*kshootMap.settings["t"]);
	lastTimingPoint->beatDuration = 60000.0 / bpm;
	lastTimingPoint->numerator = 4;

	// Block offset for current timing point
	uint32 timingPointBlockOffset = 0;
	// Tick offset into block for current timing point
	uint32 timingTickOffset = 0;
	// Duration of first timing block
	double timingFirstBlockDuration = 0.0f;

	// Add First timing point
	m_timingPoints.Add(lastTimingPoint);
	timingPointMap.Add(lastTimingPoint->time, lastTimingPoint);

	// Add First Lane Toggle Point
	LaneHideTogglePoint* startLaneTogglePoint = new LaneHideTogglePoint();
	startLaneTogglePoint->time = 0;
	startLaneTogglePoint->duration = 1;
	m_laneTogglePoints.Add(startLaneTogglePoint);

	// Stop here if we're only going for metadata
	if (metadataOnly)
		return true;

	// Button hold states
	TempButtonState* buttonStates[6] = { nullptr };
	// Laser segment states
	TempLaserState* laserStates[2] = { nullptr };

	EffectType currentButtonEffectTypes[2] = { EffectType::None };
	// 2 per button
	int16 currentButtonEffectParams[4] = { -1 };
	const uint32 maxEffectParamsPerButtons = 2;
	float laserRanges[2] = { 1.0f, 1.0f };

	uint8 sampleIndex = 0;


	for (KShootMap::TickIterator it(kshootMap); it; ++it)
	{
		const KShootBlock& block = it.GetCurrentBlock();
		KShootTime time = it.GetTime();
		const KShootTick& tick = *it;

		// Calculate MapTime from current tick
		double blockDuration = lastTimingPoint->GetBarDuration();
		uint32 blockFromStartOfTimingPoint = (time.block - timingPointBlockOffset);
		uint32 tickFromStartOfTimingPoint;

		if (blockFromStartOfTimingPoint == 0) // Use tick offset when in first block
			tickFromStartOfTimingPoint = (time.tick - timingTickOffset);
		else
			tickFromStartOfTimingPoint = time.tick;

		// Get the offset calculated by adding block durations together
		double blockDurationOffset = 0;
		if (timingTickOffset > 0) // First block might have a shorter length because of the timing point being mid tick
		{
			if (blockFromStartOfTimingPoint > 0)
				blockDurationOffset = timingFirstBlockDuration + blockDuration * (blockFromStartOfTimingPoint - 1);
		}
		else
		{
			blockDurationOffset = blockDuration * blockFromStartOfTimingPoint;
		}

		// Sub-Block offset by adding ticks together
		double blockPercent = (double)tickFromStartOfTimingPoint / (double)block.ticks.size();
		double tickOffset = blockPercent * blockDuration;
		MapTime mapTime = lastTimingPoint->time + MapTime(blockDurationOffset + tickOffset);

		bool lastTick = &block == &kshootMap.blocks.back() &&
			&tick == &block.ticks.back();

		// flag set when a new effect parameter is set and a new hold notes should be created
		bool splitupHoldNotes = false;

		// Process settings
		for (auto& p : tick.settings)
		{
			// Functions that adds a new timing point at current location if it's not yet there
			auto AddTimingPoint = [&](double newDuration, uint32 newNum, uint32 newDenom)
			{
				// Does not yet exist at current time?
				if (!timingPointMap.Contains(mapTime))
				{
					lastTimingPoint = new TimingPoint(*lastTimingPoint);
					lastTimingPoint->time = mapTime;
					m_timingPoints.Add(lastTimingPoint);
					timingPointMap.Add(mapTime, lastTimingPoint);
					timingPointBlockOffset = time.block;
					timingTickOffset = time.tick;
				}

				lastTimingPoint->numerator = newNum;
				lastTimingPoint->denominator = newDenom;
				lastTimingPoint->beatDuration = newDuration;

				// Calculate new block duration
				blockDuration = lastTimingPoint->GetBarDuration();

				// Set new first block duration based on remaining ticks
				timingFirstBlockDuration = (double)(block.ticks.size() - time.tick) / (double)block.ticks.size() * blockDuration;
			};

			// Parser the effect and parameters of an FX button (1.60)
			auto ParseFXAndParameters = [&](String in, int16* paramsOut)
			{
				// Clear parameters
				memset(paramsOut, -1, sizeof(uint16) * maxEffectParamsPerButtons);

				String effectName = in;
				size_t paramSplit = in.find_first_of(';');
				if (paramSplit != -1)
					effectName = effectName.substr(0, paramSplit);
				effectName.Trim();

				// Clear effect instead?
				if (effectName.empty())
					return EffectType::None;

				const  EffectType* type = effectTypeMap.FindEffectType(effectName);
				if (type == nullptr)
				{
					Logf("Invalid custom effect name in ksh map: %s", Logger::Warning, effectName);
					return EffectType::None;
				}

				if (paramSplit != -1)
				{
					String paramA, paramB;
					String effectParams = p.second.substr(paramSplit + 1);
					if (effectParams.Split(";", &paramA, &paramB))
					{
						paramsOut[0] = atoi(*paramA);
						paramsOut[1] = atoi(*paramB);
					}
					else
						paramsOut[0] = atoi(*effectParams);
				}
				return *type;
			};

			if (p.first == "beat")
			{
				String n, d;
				if (!p.second.Split("/", &n, &d))
					assert(false);
				uint32 num = atol(*n);
				uint32 denom = atol(*d);
				//assert(denom % 4 == 0);

				AddTimingPoint(lastTimingPoint->beatDuration, num, denom);
			}
			else if (p.first == "t")
			{
				double bpm = atof(*p.second);
				AddTimingPoint(60000.0 / bpm, lastTimingPoint->numerator, lastTimingPoint->denominator);
			}
			else if (p.first == "laserrange_l")
			{
				laserRanges[0] = 2.0f;
			}
			else if (p.first == "laserrange_r")
			{
				laserRanges[1] = 2.0f;
			}
			else if (p.first == "fx-l") // KSH 1.6
			{
				currentButtonEffectTypes[0] = ParseFXAndParameters(p.second, currentButtonEffectParams);
				splitupHoldNotes = true;
			}
			else if (p.first == "fx-r") // KSH 1.6
			{
				currentButtonEffectTypes[1] = ParseFXAndParameters(p.second, currentButtonEffectParams + maxEffectParamsPerButtons);
				splitupHoldNotes = true;
			}
			else if (p.first == "fx-l_param1")
			{
				currentButtonEffectParams[0] = atoi(*p.second);
				splitupHoldNotes = true;
			}
			else if (p.first == "fx-r_param1")
			{
				currentButtonEffectParams[maxEffectParamsPerButtons] = atoi(*p.second);
				splitupHoldNotes = true;
			}
			else if (p.first == "filtertype")
			{
				// Inser filter type change event
				EventObjectState* evt = new EventObjectState();
				evt->time = mapTime;
				evt->key = EventKey::LaserEffectType;
				evt->data.effectVal = ParseFilterType(p.second);
				m_objectStates.Add(*evt);
			}
			else if (p.first == "pfiltergain")
			{
				// Inser filter type change event
				float gain = (float)atol(*p.second) / 100.0f;
				EventObjectState* evt = new EventObjectState();
				evt->time = mapTime;
				evt->key = EventKey::LaserEffectMix;
				evt->data.floatVal = gain;
				m_objectStates.Add(*evt);
			}
			else if (p.first == "chokkakuvol")
			{
				float vol = (float)atol(*p.second) / 100.0f;
				EventObjectState* evt = new EventObjectState();
				evt->time = mapTime;
				evt->key = EventKey::LaserEffectMix;
				evt->data.floatVal = vol;
				m_objectStates.Add(*evt);
			}
			else if (p.first == "zoom_bottom")
			{
				ZoomControlPoint* point = new ZoomControlPoint();
				point->time = mapTime;
				point->index = 0;
				point->zoom = (float)atol(*p.second) / 100.0f;
				m_zoomControlPoints.Add(point);
			}
			else if (p.first == "zoom_top")
			{
				ZoomControlPoint* point = new ZoomControlPoint();
				point->time = mapTime;
				point->index = 1;
				point->zoom = (float)atol(*p.second) / 100.0f;
				m_zoomControlPoints.Add(point);
			}
			else if (p.first == "lane_toggle")
			{
				LaneHideTogglePoint* point = new LaneHideTogglePoint();
				point->time = mapTime;
				point->duration = atol(*p.second);
				m_laneTogglePoints.Add(point);
			}
			else if (p.first == "tilt")
			{
				EventObjectState* evt = new EventObjectState();
				evt->time = mapTime;
				evt->key = EventKey::TrackRollBehaviour;
				evt->data.rollVal = TrackRollBehaviour::Zero;
				String v = p.second;
				size_t f = v.find("keep_");
				if (f != -1)
				{
					evt->data.rollVal = TrackRollBehaviour::Keep;
					v = v.substr(f + 5);
				}

				if (v == "normal")
				{
					evt->data.rollVal = evt->data.rollVal | TrackRollBehaviour::Normal;
				}
				else if (v == "bigger")
				{
					evt->data.rollVal = evt->data.rollVal | TrackRollBehaviour::Bigger;
				}
				else if (v == "biggest")
				{
					evt->data.rollVal = evt->data.rollVal | TrackRollBehaviour::Biggest;
				}

				m_objectStates.Add(*evt);
			}
			else if (p.first == "fx_sample")
			{
				auto it = std::find(m_samplePaths.begin(), m_samplePaths.end(), p.second);
				if (it == m_samplePaths.end())
				{
					sampleIndex = m_samplePaths.size();
					m_samplePaths.Add(p.second);
				}
				else
				{
					sampleIndex = std::distance(m_samplePaths.begin(), it);
				}
			}
			else if (p.first == "stop")
			{
				ChartStop* cs = new ChartStop();
				cs->time = mapTime;
				cs->duration = (atol(*p.second) / 192.0f) * (lastTimingPoint->beatDuration) * 4;
				m_chartStops.Add(cs);
			}
			else
			{
				Logf("[KSH]Unkown map parameter at %d:%d: %s", Logger::Warning, it.GetTime().block, it.GetTime().tick, p.first);
			}
		}

		// Set button states
		for (uint32 i = 0; i < 6; i++)
		{
			char c = i < 4 ? tick.buttons[i] : tick.fx[i - 4];
			TempButtonState*& state = buttonStates[i];
			HoldObjectState* lastHoldObject = nullptr;

			auto IsHoldState = [&]()
			{
				return state && state->numTicks > 0 && state->fineSnap;
			};
			auto CreateButton = [&]()
			{
				if (IsHoldState())
				{
					HoldObjectState* obj = lastHoldObject = new HoldObjectState();
					obj->time = state->startTime;
					obj->index = i;
					obj->duration = mapTime - state->startTime;
					obj->effectType = state->effectType;
					if (state->lastHoldObject)
						state->lastHoldObject->next = obj;
					obj->prev = state->lastHoldObject;
					memcpy(obj->effectParams, state->effectParams, sizeof(state->effectParams));
					m_objectStates.Add(*obj);
				}
				else
				{
					ButtonObjectState* obj = new ButtonObjectState();
					
					obj->time = state->startTime;
					obj->index = i;
					obj->hasSample = state->usingSample;
					obj->sampleIndex = state->sampleIndex;
					m_objectStates.Add(*obj);
				}

				// Reset 
				delete state;
				state = nullptr;
			};

			// Split up multiple hold notes
			if (IsHoldState() && splitupHoldNotes)
			{
				CreateButton();
			}

			if (c == '0')
			{
				// Terminate hold button
				if (state)
				{
					CreateButton();
				}

				if (i >= 4)
				{
					// Unset effect parameters
					currentButtonEffectParams[i - 4] = -1;
				}
			}
			else if (!state)
			{
				// Create new hold state
				state = new TempButtonState(mapTime);
				uint32 div = (uint32)block.ticks.size();

				if (lastHoldObject)
					state->lastHoldObject = lastHoldObject;

				if (i < 4)
				{
					// Normal '1' notes are always individual
					state->fineSnap = c != '1';
				}
				else
				{
					// FX object '2' is always individual
					// FX objext '3' is like '2' but with sound sample
					state->fineSnap = c != '2' && c != '3';

					// Set effect
					if (c == 'B')
					{
						state->effectType = EffectType::Bitcrush;
						if (currentButtonEffectParams[i - 4] != -1)
							state->effectParams[0] = currentButtonEffectParams[i - 4];
						else
							state->effectParams[0] = 5;
					}
					else if (c >= 'G' && c <= 'L') // Gate 4/8/16/32/12/24
					{
						state->effectType = EffectType::Gate;
						int16 paramMap[] = {
							4, 8, 16, 32, 12, 24
						};
						state->effectParams[0] = paramMap[c - 'G'];
					}
					else if (c >= 'S' && c <= 'W') // Retrigger 8/16/32/12/24
					{
						state->effectType = EffectType::Retrigger;
						int16 paramMap[] = {
							8, 16, 32, 12, 24
						};
						state->effectParams[0] = paramMap[c - 'S'];
					}
					else if (c == 'Q')
					{
						state->effectType = EffectType::Phaser;
					}
					else if (c == 'F')
					{
						state->effectType = EffectType::Flanger;
						state->effectParams[0] = 5000;
					}
					else if (c == 'X')
					{
						state->effectType = EffectType::Wobble;
						state->effectParams[0] = 12;
					}
					else if (c == 'D')
					{
						state->effectType = EffectType::SideChain;
					}
					else if (c == 'A')
					{
						state->effectType = EffectType::TapeStop;
						if (currentButtonEffectParams[i - 4] != -1)
							memcpy(state->effectParams, currentButtonEffectParams + (i - 4) * maxEffectParamsPerButtons,
								sizeof(state->effectParams));
						else
							state->effectParams[0] = 50;
					}
					else if (c == '3')
					{
						state->usingSample = true;
						state->sampleIndex = sampleIndex;
					}
					else
					{
						// Use settings method of setting effects+params (1.60)
						state->effectType = currentButtonEffectTypes[i - 4];
						if (currentButtonEffectParams[(i - 4) * maxEffectParamsPerButtons] != -1)
							memcpy(state->effectParams, currentButtonEffectParams + (i - 4) * maxEffectParamsPerButtons,
								sizeof(state->effectParams));
						else
						{
							state->effectParams[0] = defaultEffectParams[state->effectType];
							state->effectParams[1] = 0;
						}
					}
				}
			}
			else
			{
				// For buttons not using the 1/32 grid
				if (!state->fineSnap)
				{
					CreateButton();

					// Create new hold state
					state = new TempButtonState(mapTime);
					uint32 div = (uint32)block.ticks.size();

					if (i < 4)
					{
						// Normal '1' notes are always individual
						state->fineSnap = c != '1';
					}
					else
					{
						// Hold are always on a high enough snap to make suere they are seperate when needed
						state->fineSnap = c != '2' && c != '3';
						if (c == '3')
						{
							state->usingSample = true;
							state->sampleIndex = sampleIndex;
						}
					}
				}
				else
				{
					// Update current hold state
					state->numTicks++;
				}
			}

			// Terminate last item
			if (lastTick && state)
				CreateButton();
		}

		// Set laser states
		for (uint32 i = 0; i < 2; i++)
		{
			TempLaserState*& state = laserStates[i];
			char c = tick.laser[i];

			// Function that creates a new segment out of the current state
			auto CreateLaserSegment = [&](float endPos)
			{
				// Process existing segment
				//assert(state->numTicks > 0);

				LaserObjectState* obj = new LaserObjectState();

				obj->time = state->startTime;
				obj->duration = mapTime - state->startTime;
				obj->index = i;
				obj->points[0] = state->startPosition;
				obj->points[1] = endPos;



				if (laserRanges[i] > 1.0f)
				{
					obj->flags |= LaserObjectState::flag_Extended;
				}
				// Threshold for laser segments to be considered instant
				MapTime laserSlamThreshold = (MapTime)ceil(state->tpStart->beatDuration / 8.0);
				if (obj->duration <= laserSlamThreshold && (obj->points[1] != obj->points[0]))
				{
					obj->flags |= LaserObjectState::flag_Instant;
					if (state->spinType != 0)
					{
						obj->spin.duration = state->spinDuration;
						switch (state->spinType)
						{
						case '(':
						case ')':
							obj->spin.type = SpinStruct::SpinType::Full;
							break;
						case '<':
						case '>':
							obj->spin.type = SpinStruct::SpinType::Quarter;
							break;
						default:
							break;
						}
						switch (state->spinType)
						{
						case '<':
						case '(':
							obj->spin.direction = -1.0f;
							break;
						case ')':
						case '>':
							obj->spin.direction = 1.0f;
							break;
						default:
							break;
						}
					}
				}

				// Link segments together
				if (state->last)
				{
					// Always fixup duration so they are connected by duration as well
					obj->prev = state->last;
					MapTime actualPrevDuration = obj->time - obj->prev->time;
					if (obj->prev->duration != actualPrevDuration)
					{
						obj->prev->duration = actualPrevDuration;
					}
					obj->prev->next = obj;

				}

				// Add to list of objects
				m_objectStates.Add(*obj);

				return obj;
			};

			if (c == '-')
			{
				// Terminate laser
				if (state)
				{
					// Reset state
					delete state;
					state = nullptr;

					// Reset range extension
					laserRanges[i] = 1.0f;
				}
			}
			else if (c == ':')
			{
				// Update current laser state
				if (state)
				{
					state->numTicks++;
				}
			}
			else
			{
				float pos = kshootMap.TranslateLaserChar(c);
				LaserObjectState* last = nullptr;
				if (state)
				{
					last = CreateLaserSegment(pos);

					// Reset state
					delete state;
					state = nullptr;
				}

				MapTime startTime = mapTime;
				if (last && (last->flags & LaserObjectState::flag_Instant) != 0)
				{
					// Move offset to be the same as last segment, as in ksh maps there is a 1 tick delay after laser slams
					startTime = last->time;
				}
				state = new TempLaserState(startTime, 0, lastTimingPoint);
				state->last = last; // Link together
				state->startPosition = pos;

				//@[Type][Speed] = spin
				//Types
				//) or ( = full spin
				//> or < = quarter spin
				//Speed is number of 192nd notes
				if (!tick.add.empty() && tick.add[0] == '@')
				{
					state->spinType = tick.add[1];
					state->spinDuration = std::stoi(tick.add.substr(2));
					if(state->spinType == '(' || state->spinType == ')')
						state->spinDuration = (3 * std::stoi(tick.add.substr(2))) / 4;
				}

			}
		}
	}

	//// Split laser segments on bpm changes

	//Vector<ObjectState*> newLasers;
	//for (ObjectState* it : m_objectStates)
	//{
	//	if (it->type == ObjectType::Laser)
	//	{
	//		LaserObjectState* laser = (LaserObjectState*)it;
	//		if (laser->flags & LaserObjectState::flag_Instant)
	//			continue;
	//		for (TimingPoint* tp : m_timingPoints)
	//		{
	//			if (laser->time < tp->time && laser->time + laser->duration > tp->time)
	//			{
	//				LaserObjectState* obj = new LaserObjectState();

	//				obj->time = tp->time;
	//				obj->duration = (laser->time + laser->duration) - tp->time;
	//				obj->index = laser->index;
	//				obj->flags = laser->flags;
	//				float meetingPoint = (float)(laser->duration - obj->duration) / laser->duration;
	//				meetingPoint = laser->points[0] + (meetingPoint * (laser->points[1] - laser->points[0]));
	//				obj->points[0] = meetingPoint;
	//				obj->points[1] = laser->points[1];
	//				laser->points[1] = meetingPoint;
	//				laser->duration -= obj->duration;
	//				
	//				obj->next = laser->next;
	//				obj->prev = laser;
	//				if (laser->next)
	//					laser->next->prev = obj;
	//				laser->next = obj;
	//				newLasers.Add(*obj);
	//			}
	//		}
	//	}
	//}

	//m_objectStates.reserve(newLasers.size());
	//m_objectStates.insert(m_objectStates.end(), newLasers.begin(), newLasers.end());

	// Re-sort collection to fix some inconsistencies caused by corrections after laser slams
	ObjectState::SortArray(m_objectStates);

	return true;
}