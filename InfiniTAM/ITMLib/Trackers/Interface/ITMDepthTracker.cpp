// Copyright 2014-2015 Isis Innovation Limited and the authors of InfiniTAM

#include "ITMDepthTracker.h"
#include "../../../ORUtils/Cholesky.h"

#include <math.h>

using namespace ITMLib;

ITMDepthTracker::ITMDepthTracker(Vector2i imgSize, TrackerIterationType *trackingRegime, int noHierarchyLevels,
	float terminationThreshold, const ITMLowLevelEngine *lowLevelEngine, MemoryDeviceType memoryType)
{
	viewHierarchy = new ITMImageHierarchy<ITMTemplatedHierarchyLevel<ITMFloatImage> >(imgSize, trackingRegime, noHierarchyLevels, memoryType, true);
	sceneHierarchy = new ITMImageHierarchy<ITMSceneHierarchyLevel>(imgSize, trackingRegime, noHierarchyLevels, memoryType, true);

	this->noIterationsPerLevel = new int[noHierarchyLevels];
	this->distThresh = new float[noHierarchyLevels];

	SetupLevels(noHierarchyLevels*2, 2, 0.01f, 0.002f);

	this->lowLevelEngine = lowLevelEngine;

	this->terminationThreshold = terminationThreshold;
}

ITMDepthTracker::~ITMDepthTracker(void) 
{ 
	delete this->viewHierarchy;
	delete this->sceneHierarchy;

	delete[] this->noIterationsPerLevel;
	delete[] this->distThresh;
}

void ITMDepthTracker::SetupLevels(int numIterCoarse, int numIterFine, float distThreshCoarse, float distThreshFine)
{
	int noHierarchyLevels = viewHierarchy->noLevels;

	if ((numIterCoarse != -1) && (numIterFine != -1)) {
		float step = (float)(numIterCoarse - numIterFine) / (float)(noHierarchyLevels - 1);
		float val = (float)numIterCoarse;
		for (int levelId = noHierarchyLevels - 1; levelId >= 0; levelId--) {
			this->noIterationsPerLevel[levelId] = (int)round(val);
			val -= step;
		}
	}
	if ((distThreshCoarse >= 0.0f) && (distThreshFine >= 0.0f)) {
		float step = (float)(distThreshCoarse - distThreshFine) / (float)(noHierarchyLevels - 1);
		float val = distThreshCoarse;
		for (int levelId = noHierarchyLevels - 1; levelId >= 0; levelId--) {
			this->distThresh[levelId] = val;
			val -= step;
		}
	}
}

void ITMDepthTracker::SetEvaluationData(ITMTrackingState *trackingState, const ITMView *view)
{
	this->trackingState = trackingState;
	this->view = view;

	sceneHierarchy->levels[0]->intrinsics = view->calib->intrinsics_d.projectionParamsSimple.all;
	viewHierarchy->levels[0]->intrinsics = view->calib->intrinsics_d.projectionParamsSimple.all;

	// the image hierarchy allows pointers to external data at level 0
	viewHierarchy->levels[0]->depth = view->depth;
	sceneHierarchy->levels[0]->pointsMap = trackingState->pointCloud->locations;
	sceneHierarchy->levels[0]->normalsMap = trackingState->pointCloud->colours;

	scenePose = trackingState->pose_pointCloud->GetM();
}

void ITMDepthTracker::PrepareForEvaluation()
{
	for (int i = 1; i < viewHierarchy->noLevels; i++)
	{
		ITMTemplatedHierarchyLevel<ITMFloatImage> *currentLevelView = viewHierarchy->levels[i], *previousLevelView = viewHierarchy->levels[i - 1];
		lowLevelEngine->FilterSubsampleWithHoles(currentLevelView->depth, previousLevelView->depth);
		currentLevelView->intrinsics = previousLevelView->intrinsics * 0.5f;

		ITMSceneHierarchyLevel *currentLevelScene = sceneHierarchy->levels[i], *previousLevelScene = sceneHierarchy->levels[i - 1];
		//lowLevelEngine->FilterSubsampleWithHoles(currentLevelScene->pointsMap, previousLevelScene->pointsMap);
		//lowLevelEngine->FilterSubsampleWithHoles(currentLevelScene->normalsMap, previousLevelScene->normalsMap);
		currentLevelScene->intrinsics = previousLevelScene->intrinsics * 0.5f;
	}
}

void ITMDepthTracker::SetEvaluationParams(int levelId)
{
	this->levelId = levelId;
	this->iterationType = viewHierarchy->levels[levelId]->iterationType;
	this->sceneHierarchyLevel = sceneHierarchy->levels[0];
	this->viewHierarchyLevel = viewHierarchy->levels[levelId];
}

void ITMDepthTracker::ComputeDelta(float *step, float *nabla, float *hessian, bool shortIteration) const
{
	for (int i = 0; i < 6; i++) step[i] = 0;

	if (shortIteration)
	{
		float smallHessian[3 * 3];
		for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) smallHessian[r + c * 3] = hessian[r + c * 6];

		ORUtils::Cholesky cholA(smallHessian, 3);
		cholA.Backsub(step, nabla);
	}
	else
	{
		ORUtils::Cholesky cholA(hessian, 6);
		cholA.Backsub(step, nabla);
	}
}

bool ITMDepthTracker::HasConverged(float *step) const
{
	float stepLength = 0.0f;
	for (int i = 0; i < 6; i++) stepLength += step[i] * step[i];

	if (sqrt(stepLength) / 6 < terminationThreshold) return true; //converged

	return false;
}

void ITMDepthTracker::ApplyDelta(const Matrix4f & para_old, const float *delta, Matrix4f & para_new) const
{
	float step[6];

	switch (iterationType)
	{
	case TRACKER_ITERATION_ROTATION:
		step[0] = (float)(delta[0]); step[1] = (float)(delta[1]); step[2] = (float)(delta[2]);
		step[3] = 0.0f; step[4] = 0.0f; step[5] = 0.0f;
		break;
	case TRACKER_ITERATION_TRANSLATION:
		step[0] = 0.0f; step[1] = 0.0f; step[2] = 0.0f;
		step[3] = (float)(delta[0]); step[4] = (float)(delta[1]); step[5] = (float)(delta[2]);
		break;
	default:
	case TRACKER_ITERATION_BOTH:
		step[0] = (float)(delta[0]); step[1] = (float)(delta[1]); step[2] = (float)(delta[2]);
		step[3] = (float)(delta[3]); step[4] = (float)(delta[4]); step[5] = (float)(delta[5]);
		break;
	}

	Matrix4f Tinc;

	Tinc.m00 = 1.0f;		Tinc.m10 = step[2];		Tinc.m20 = -step[1];	Tinc.m30 = step[3];
	Tinc.m01 = -step[2];	Tinc.m11 = 1.0f;		Tinc.m21 = step[0];		Tinc.m31 = step[4];
	Tinc.m02 = step[1];		Tinc.m12 = -step[0];	Tinc.m22 = 1.0f;		Tinc.m32 = step[5];
	Tinc.m03 = 0.0f;		Tinc.m13 = 0.0f;		Tinc.m23 = 0.0f;		Tinc.m33 = 1.0f;

	para_new = Tinc * para_old;
}

void ITMDepthTracker::TrackCamera(ITMTrackingState *trackingState, const ITMView *view)
{
	this->SetEvaluationData(trackingState, view);
	this->PrepareForEvaluation();

	float f_old = 1e10, f_new;
	int noValidPoints_new;
	int noValidPoints_old = 0;
	//int noTotalPoints = 0;

	float hessian_good[6 * 6], hessian_new[6 * 6], A[6 * 6];
	float nabla_good[6], nabla_new[6];
	float step[6];

	for (int i = 0; i < 6*6; ++i) hessian_good[i] = 0.0f;
	for (int i = 0; i < 6; ++i) nabla_good[i] = 0.0f;

	for (int levelId = viewHierarchy->noLevels - 1; levelId >= 0; levelId--)
	{
		this->SetEvaluationParams(levelId);
		if (iterationType == TRACKER_ITERATION_NONE) continue;

		//noTotalPoints = viewHierarchy->levels[levelId]->depth->noDims.x * viewHierarchy->levels[levelId]->depth->noDims.y;

		Matrix4f approxInvPose = trackingState->pose_d->GetInvM();
		ORUtils::SE3Pose lastKnownGoodPose(*(trackingState->pose_d));
		f_old = 1e20f;
		noValidPoints_old = 0;
		float lambda = 1.0;

		for (int iterNo = 0; iterNo < noIterationsPerLevel[levelId]; iterNo++)
		{
			// evaluate error function and gradients
			noValidPoints_new = this->ComputeGandH(f_new, nabla_new, hessian_new, approxInvPose);

			// check if error increased. If so, revert
			if ((noValidPoints_new <= 0)||(f_new > f_old)) {
				trackingState->pose_d->SetFrom(&lastKnownGoodPose);
				approxInvPose = trackingState->pose_d->GetInvM();
				lambda *= 10.0f;
			} else {
				lastKnownGoodPose.SetFrom(trackingState->pose_d);
				f_old = f_new;
				noValidPoints_old = noValidPoints_new;

				for (int i = 0; i < 6*6; ++i) hessian_good[i] = hessian_new[i] / noValidPoints_new;
				for (int i = 0; i < 6; ++i) nabla_good[i] = nabla_new[i] / noValidPoints_new;
				lambda /= 10.0f;
			}
			for (int i = 0; i < 6*6; ++i) A[i] = hessian_good[i];
			for (int i = 0; i < 6; ++i) A[i+i*6] *= 1.0f + lambda;

			// compute a new step and make sure we've got an SE3
			ComputeDelta(step, nabla_good, A, iterationType != TRACKER_ITERATION_BOTH);
			ApplyDelta(approxInvPose, step, approxInvPose);
			trackingState->pose_d->SetInvM(approxInvPose);
			trackingState->pose_d->Coerce();
			approxInvPose = trackingState->pose_d->GetInvM();

			// if step is small, assume it's going to decrease the error and finish
			if (HasConverged(step)) break;
		}
	}
#if 1

	int noValidPointsMax = lowLevelEngine->CountValidDepths(view->depth);

#if 0
	float det_ref, det_ref_norm;
	{
		float h_ref[6*6];
		int noPoints_ref = this->ComputeHessianAtMinimum(h_ref);
//		float f_ref, g_ref[6], h_ref[6*6];
//		int noPoints_ref = this->ComputeGandH(f_ref, g_ref, h_ref, /*trackingState->pose_pointCloud->GetInvM()*/scenePose);
for (int i = 0; i < 6*6; ++i) h_ref[i] /= (float)noPoints_ref;
{		ORUtils::Cholesky cholRef(h_ref, 6);
		det_ref = cholRef.Determinant();}
for (int i = 0; i < 6*6; ++i) h_ref[i] *= (float)noPoints_ref/(float)noTotalPoints;
{		ORUtils::Cholesky cholRef(h_ref, 6);
		det_ref_norm = cholRef.Determinant();}
		//fprintf(stderr, "reference: %e %i, g %e %e %e %e %e %e det %e\n", f_ref, noPoints_ref, g_ref[0], g_ref[1], g_ref[2], g_ref[3], g_ref[4], g_ref[5], det_ref);
/*for (int r = 0; r < 6; r++) { for (int c = 0; c < 6; c++) fprintf(stderr, "%f ", hessian_good[r*6+c]); fprintf(stderr, "\n"); }
for (int r = 0; r < 6; r++) { for (int c = 0; c < 6; c++) fprintf(stderr, "%f ", h_ref[r*6+c]); fprintf(stderr, "\n"); }
		fprintf(stderr, "reference: %i, det %e\n", noPoints_ref, det_ref);*/
	}

	float det = 0.0f, det_norm;
	if (iterationType == TRACKER_ITERATION_BOTH) {
		ORUtils::Cholesky cholA(hessian_good, 6);
		det = cholA.Determinant();
	}
//for (int i = 0; i < 6*6; ++i) hessian_good[i] *= (float)noValidPoints_old/(float)noTotalPoints;
#endif
	float det_norm = 0.0f;
	if (iterationType == TRACKER_ITERATION_BOTH) {
		for (int i = 0; i < 6*6; ++i) hessian_good[i] *= (float)noValidPoints_old/(float)noValidPointsMax;
		ORUtils::Cholesky cholA(hessian_good, 6);
		det_norm = cholA.Determinant();
	}

	//float finalResidual = sqrt(((float)noValidPoints_old * f_old + (float)(noTotalPoints - noValidPoints_old) * distThresh[0]) / (float)noTotalPoints);
	float finalResidual = sqrt(((float)noValidPoints_old * f_old + (float)(noValidPointsMax - noValidPoints_old) * distThresh[0]) / (float)noValidPointsMax);
	//float percentageInliers = (float)noValidPoints_old/(float)noTotalPoints;
	float percentageInliers = (float)noValidPoints_old/(float)noValidPointsMax;
//fprintf(stderr, "final ICP residual: det %e, r %f i %f (r %e, p %i/%i)\n", det, finalResidual, percentageInliers, f_old, noValidPoints_old, noValidPointsMax);
//fprintf(stderr, " final ICP det ratio: raw %f %f (%e/%e) norm %f %f (%e %e)\n", det/det_ref, log(det)/log(det_ref), det, det_ref, det_norm/det_ref_norm, log(det_norm)/log(det_ref_norm), det_norm, det_ref_norm);
//fprintf(stderr, "residual %f inliers %f\n", finalResidual, percentageInliers);
/*	trackingState->poseQuality = 1.0f;
	if (finalResidual > 0.2f) trackingState->poseQuality *= 0.5f;
	if (percentageInliers < 0.8f) trackingState->poseQuality *= 0.5f;
	if (det_norm < exp(-35.0f)) trackingState->poseQuality *= 0.5f;

	if (percentageInliers < 0.3f) trackingState->poseQuality = 0.0f;*/
	static const float thresholdResidual = 0.25f; // 0.2f
	static const float thresholdInliers = 0.7f; // 0.8f
	static const float thresholdDet = exp(-36.0f); //exp(-35.0f)
	if ((finalResidual < thresholdResidual)&&(percentageInliers > thresholdInliers)) {
		trackingState->poseQuality = 1.0f;
	} else if ((finalResidual < thresholdResidual)||(percentageInliers > thresholdInliers)) {
		trackingState->poseQuality = 0.5f;
	} else {
		trackingState->poseQuality = 0.0f;
	}
	if (percentageInliers < 0.3f) trackingState->poseQuality = 0.0f;
	if (det_norm<thresholdDet) if (trackingState->poseQuality > 0.5f) trackingState->poseQuality = 0.5f;
//if (det_norm<exp(-35.0f)) fprintf(stderr, "determinant poor! %f, %e %e\n", log(det_norm), det_norm, exp(-35.0f));*/
//fprintf(stderr, "tracking quality: %f\n", trackingState->poseQuality);
#endif
}
