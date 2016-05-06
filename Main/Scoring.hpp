#pragma once
#include "BeatmapPlayback.hpp"

// Hit rating for hitting a button
enum class ScoreHitRating
{
	Miss = 0,
	Good,
	Perfect,
};

// Recorded button hit state for a single ObjectState
struct ObjectHitState
{
	int64 hitDeltas[6];
	uint32 mask;
};
struct HitStat
{
	HitStat(MapTime time, MapTime delta) : time(time), delta(delta) {};
	bool operator<(const HitStat& other) { return time < other.time; }
	MapTime time;
	MapTime delta;
};

/*
	Calculates game score and checks which objects are hit
	also keeps track of laser positions
*/
class Scoring : public Unique
{
public:
	Scoring();

	// Needs to be set to find out which objects are active/hittable
	void SetPlayback(BeatmapPlayback& playback);

	// Updates the list of objects that are possible to hit
	void Tick(float deltaTime);

	// Score for hiting an object with given delta
	ScoreHitRating GetHitRatingFromDelta(MapTime delta);
	// Delta of object as if it were hit right now
	MapTime GetObjectHitDelta(ObjectState* obj);

	// Processed button press events, returns the hit object, if any
	ObjectState* OnButtonPressed(uint32 buttonCode);
	// Processed button release events
	void OnButtonReleased(uint32 buttonCode);

	// Check if a hold object or laser is currently being held
	bool IsActive(ObjectState* object) const;

	// Get laser roll values
	// these range from 0 to 1, 0 being the the laser at home position, and 1 the outer extreme
	float GetActiveLaserRoll(uint32 index);

	Delegate<uint32, MapTime> OnButtonScore;
	Delegate<uint32, ObjectState*> OnButtonHit;
	Delegate<uint32> OnButtonMiss;
	Delegate<uint32> OnLaserSlamHit;

	// The maximum timing window in which you are able to hit an object early
	static const MapTime maxEarlyHitTime;
	static const MapTime perfectHitTime;
	// Maximum timing window in which you can hit laser segments
	static const MapTime maxLaserHitTime;

	// Movement speed of the lasers when idle
	static const float idleLaserMoveSpeed;

	// Maximum accumulated score of object that have been hit or missed
	uint32 currentMaxScore;
	// The actual amount of gotten score
	uint32 currentHitScore;

	// Current combo
	uint32 currentComboCounter;

	// The timings of hit objects, sorted by time hit
	// this is mainly for debugging purposes
	Vector<HitStat> hitStats;

	// Autoplay mode
	bool autoplay = false;

	// Laser Objects
	float laserInput[2];
	float laserPositions[2];
	float laserTargetPositions[2] = { 0 };
	// Always points to the currently active laser segment
	LaserObjectState* activeLaserObjects[2] = { 0 };

	// Used for hold logic on laser segments (non-instant segments)
	LaserObjectState* laserHoldObjects[2] = { 0 };
	MapTime laserHoldDuration[2] = { 0 };
	MapTime laserMissDuration[2] = { 0 };
	bool laserActive[2] = { false };

	// Hold Objects
	HoldObjectState* activeHoldObjects[6] = { 0 };
	MapTime lastHoldDuration[6] = { 0 };

private:
	void m_RegisterHit(ObjectState* obj);
	void m_OnObjectEntered(ObjectState* obj);
	void m_OnObjectLeaved(ObjectState* obj);
	float m_SampleLaserPosition(MapTime time, LaserObjectState* laser);

	// For hold note ticks
	uint32 m_holdTickCounter;
	MapTime m_lastTime;

	// Used to calculate average hit delay
	int64 m_hitNotesDelta;
	int64 m_numNotesHit;
	class BeatmapPlayback* m_playback;
};
