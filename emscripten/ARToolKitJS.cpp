#include <stdio.h>
#include <AR/ar.h>
#include <AR/gsub_lite.h>
// #include <AR/gsub_es2.h>
#include <AR/arMulti.h>
#include <emscripten.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <AR/config.h>
#include <AR/arFilterTransMat.h>
#include <AR2/tracking.h>
#include <KPM/kpm.h>

struct multi_marker {
	int id;
	ARMultiMarkerInfoT *multiMarkerHandle;
};

struct arController {
	int id;

	ARParam param;
	ARParamLT *paramLT = NULL;

	ARUint8 *videoFrame = NULL;
	int videoFrameSize;

	int width = 0;
	int height = 0;

	ARHandle *arhandle = NULL;
	ARPattHandle *arPattHandle = NULL;
	ARMultiMarkerInfoT *arMultiMarkerHandle = NULL;
	AR3DHandle* ar3DHandle;

	KpmHandle* kpmHandle;
	AR2HandleT* ar2Handle;

	int surfaceSetCount = 0; // Running NFT marker id
	std::unordered_map<int, AR2SurfaceSetT*> surfaceSets;

	ARdouble nearPlane = 0.0001;
	ARdouble farPlane = 1000.0;

	std::vector<multi_marker> multi_markers;

	int patt_id = 0; // Running pattern marker id

	ARdouble cameraLens[16];

};

std::unordered_map<int, arController> arControllers;
std::unordered_map<int, ARParam> cameraParams;


// ============================================================================
//	Global variables
// ============================================================================

static ARdouble	gTransform[3][4];

static int gARControllerID = 0;
static int gCameraID = 0;

static int ARCONTROLLER_NOT_FOUND = -1;
static int MULTIMARKER_NOT_FOUND = -2;
static int MARKER_INDEX_OUT_OF_BOUNDS = -3;

static ARMarkerInfo gMarkerInfo;

extern "C" {

	/**
		NFT API bindings
	*/

	int getNFTMarkerInfo(int id, int markerIndex) {
		if (arControllers.find(id) == arControllers.end()) { return ARCONTROLLER_NOT_FOUND; }
		arController *arc = &(arControllers[id]);

		if (arc->surfaceSetCount <= markerIndex) {
			return MARKER_INDEX_OUT_OF_BOUNDS;
		}

		KpmResult *kpmResult = NULL;
		int kpmResultNum = -1;

        kpmGetResult( arc->kpmHandle, &kpmResult, &kpmResultNum );

		int i, j, k;
        int flag = -1;
        float err = -1;
        float trans[3][4];
        for( i = 0; i < kpmResultNum; i++ ) {
            if (kpmResult[i].pageNo == markerIndex && kpmResult[i].camPoseF == 0 ) {
	            if( flag == -1 || err > kpmResult[i].error ) { // Take the first or best result.
	                flag = i;
	                err = kpmResult[i].error;
	            }
	        }
        }

        if (flag > -1) {
            for (j = 0; j < 3; j++) {
            	for (k = 0; k < 4; k++) {
            		trans[j][k] = kpmResult[flag].camPose[j][k];
            	}
            }
			EM_ASM_({
				var $a = arguments;
				var i = 0;
				if (!artoolkit["NFTMarkerInfo"]) {
					artoolkit["NFTMarkerInfo"] = ({
						id: 0,
						error: -1,
						found: 0,
						pose: [0,0,0,0, 0,0,0,0, 0,0,0,0]
					});
				}
				var markerInfo = artoolkit["NFTMarkerInfo"];
				markerInfo["id"] = $a[i++];
				markerInfo["error"] = $a[i++];
				markerInfo["found"] = 1;
				markerInfo["pose"][0] = $a[i++];
				markerInfo["pose"][1] = $a[i++];
				markerInfo["pose"][2] = $a[i++];
				markerInfo["pose"][3] = $a[i++];
				markerInfo["pose"][4] = $a[i++];
				markerInfo["pose"][5] = $a[i++];
				markerInfo["pose"][6] = $a[i++];
				markerInfo["pose"][7] = $a[i++];
				markerInfo["pose"][8] = $a[i++];
				markerInfo["pose"][9] = $a[i++];
				markerInfo["pose"][10] = $a[i++];
				markerInfo["pose"][11] = $a[i++];
			},
				markerIndex,
				err,

				trans[0][0],
				trans[0][1],
				trans[0][2],
				trans[0][3],

				trans[1][0],
				trans[1][1],
				trans[1][2],
				trans[1][3],

				trans[2][0],
				trans[2][1],
				trans[2][2],
				trans[2][3]
			);
        } else {
			EM_ASM_({
				var $a = arguments;
				var i = 0;
				if (!artoolkit["NFTMarkerInfo"]) {
					artoolkit["NFTMarkerInfo"] = ({
						id: 0,
						error: -1,
						found: 0,
						pose: [0,0,0,0, 0,0,0,0, 0,0,0,0]
					});
				}
				var markerInfo = artoolkit["NFTMarkerInfo"];
				markerInfo["id"] = $a[i++];
				markerInfo["error"] = -1;
				markerInfo["found"] = 0;
				markerInfo["pose"][0] = 0;
				markerInfo["pose"][1] = 0;
				markerInfo["pose"][2] = 0;
				markerInfo["pose"][3] = 0;
				markerInfo["pose"][4] = 0;
				markerInfo["pose"][5] = 0;
				markerInfo["pose"][6] = 0;
				markerInfo["pose"][7] = 0;
				markerInfo["pose"][8] = 0;
				markerInfo["pose"][9] = 0;
				markerInfo["pose"][10] = 0;
				markerInfo["pose"][11] = 0;
			},
				markerIndex
			);
        }

		return 0;
	}

	int detectNFTMarker(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		KpmResult *kpmResult = NULL;
		int kpmResultNum = -1;

        kpmMatching( arc->kpmHandle, arc->videoFrame );
        kpmGetResult( arc->kpmHandle, &kpmResult, &kpmResultNum );
        return kpmResultNum;
	}

	KpmHandle *createKpmHandle(ARParamLT *cparamLT) {
		KpmHandle *kpmHandle;
	    kpmHandle = kpmCreateHandle(cparamLT, AR_PIXEL_FORMAT_RGBA);
		return kpmHandle;
	}

	int getKpmImageWidth(KpmHandle *kpmHandle) {
		return kpmHandleGetXSize(kpmHandle);
	}

	int getKpmImageHeight(KpmHandle *kpmHandle) {
		return kpmHandleGetYSize(kpmHandle);
	}

	int getKpmPixelSize(KpmHandle *kpmHandle) {
		return arUtilGetPixelSize(kpmHandleGetPixelFormat(kpmHandle));
	}

	int setupAR2(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		arc->kpmHandle = createKpmHandle(arc->paramLT);

		return 0;
	}

	int loadNFTMarker(arController *arc, int surfaceSetCount, const char* datasetPathname) {
		int i, pageNo;
		AR2SurfaceSetT *surfaceSet;
		KpmRefDataSet *refDataSet, *refDataSet3;

		KpmHandle *kpmHandle = arc->kpmHandle;
		//ARLOGi("hogehoge%d\n", kpmHandle->xsize);

		kpmGetRefDataSet(kpmHandle, &refDataSet3); //TODO
		refDataSet = NULL;


		if (refDataSet3->pageNum > 0){
			// merging existing dataset
			kpmMergeRefDataSet(&refDataSet, &refDataSet3);
		}

		// Load KPM data.
		KpmRefDataSet  *refDataSet2;
		ARLOGi("Reading %s.fset3\n", datasetPathname);
		if (kpmLoadRefDataSet(datasetPathname, "fset3", &refDataSet2) < 0 ) {
			ARLOGe("Error reading KPM data from %s.fset3\n", datasetPathname);
			pageNo = -1;
			return (FALSE);
		}
		pageNo = surfaceSetCount;


		ARLOGi("  Assigned page no. %d.\n", surfaceSetCount);

		if (kpmChangePageNoOfRefDataSet(refDataSet2, KpmChangePageNoAllPages, surfaceSetCount) < 0) {
		    ARLOGe("Error: kpmChangePageNoOfRefDataSet\n");
		    return (FALSE);
		}
		ARLOGi("changed to %d.\n", refDataSet2->refPoint[0].pageNo);
		if (kpmMergeRefDataSet(&refDataSet, &refDataSet2) < 0) {
		    ARLOGe("Error: kpmMergeRefDataSet\n");
		    return (FALSE);
		}

		ARLOGi("  Merge Done.\n");

		// Load AR2 data.
		ARLOGi("Reading %s.fset\n", datasetPathname);

		if ((surfaceSet = ar2ReadSurfaceSet(datasetPathname, "fset", NULL)) == NULL ) {
		    ARLOGe("Error reading data from %s.fset\n", datasetPathname);
		}
		ARLOGi("  Done.\n");

		arc->surfaceSets[surfaceSetCount] = surfaceSet;

		if (kpmSetRefDataSet(kpmHandle, refDataSet) < 0) {
		    ARLOGe("Error: kpmSetRefDataSet\n");
		    return (FALSE);
		}

		// TODO instead of freeing in the end of Merge
		kpmDeleteRefDataSet(&refDataSet);
		kpmDeleteRefDataSet(&refDataSet2);

		ARLOGi("Loading of NFT data complete.\n");
		return (TRUE);
	}

	/***************
	 * Set Log Level
	 ****************/
	void setLogLevel(int level) {
		arLogLevel = level;
	}

	int getLogLevel() {
		return arLogLevel;
	}

	/***********
	* Teardown *
	***********/

	void deleteHandle(arController *arc) {
		if (arc->arhandle != NULL) {
			arPattDetach(arc->arhandle);
			arDeleteHandle(arc->arhandle);
			arc->arhandle = NULL;
		}
		if (arc->ar3DHandle != NULL) {
			ar3DDeleteHandle(&(arc->ar3DHandle));
			arc->ar3DHandle = NULL;
		}
		if (arc->paramLT != NULL) {
			arParamLTFree(&(arc->paramLT));
			arc->paramLT = NULL;
		}
	}

	int teardown(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		if (arc->videoFrame) {
			free(arc->videoFrame);
			arc->videoFrame = NULL;
			arc->videoFrameSize = 0;
		}

		deleteHandle(arc);

		arPattDeleteHandle(arc->arPattHandle);

		arControllers.erase(id);

		for (int i=0; i<arc->multi_markers.size(); i++) {
			arMultiFreeConfig(arc->multi_markers[i].multiMarkerHandle);
		}

		delete &arc->multi_markers;
		delete arc;

		return 0;
	}



	/*****************
	* Camera loading *
	*****************/

	int loadCamera(std::string cparam_name) {
		ARParam param;
		if (arParamLoad(cparam_name.c_str(), 1, &param) < 0) {
			ARLOGe("loadCamera(): Error loading parameter file %s for camera.\n", cparam_name.c_str());
			return -1;
		}
		int cameraID = gCameraID++;
		cameraParams[cameraID] = param;

		return cameraID;
	}

	int setCamera(int id, int cameraID) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		if (cameraParams.find(cameraID) == cameraParams.end()) { return -1; }

		arc->param = cameraParams[cameraID];

		if (arc->param.xsize != arc->width || arc->param.ysize != arc->height) {
			ARLOGw("*** Camera Parameter resized from %d, %d. ***\n", arc->param.xsize, arc->param.ysize);
			arParamChangeSize(&(arc->param), arc->width, arc->height, &(arc->param));
		}

		// ARLOGi("*** Camera Parameter ***\n");
		// arParamDisp(&(arc->param));

		deleteHandle(arc);

		if ((arc->paramLT = arParamLTCreate(&(arc->param), AR_PARAM_LT_DEFAULT_OFFSET)) == NULL) {
			ARLOGe("setCamera(): Error: arParamLTCreate.\n");
			return -1;
		}

		// ARLOGi("setCamera(): arParamLTCreated\n..%d, %d\n", (arc->paramLT->param).xsize, (arc->paramLT->param).ysize);

		// setup camera
		if ((arc->arhandle = arCreateHandle(arc->paramLT)) == NULL) {
			ARLOGe("setCamera(): Error: arCreateHandle.\n");
			return -1;
		}
		// AR_DEFAULT_PIXEL_FORMAT
		int set = arSetPixelFormat(arc->arhandle, AR_PIXEL_FORMAT_RGBA);

		// ARLOGi("setCamera(): arCreateHandle done\n");

		arc->ar3DHandle = ar3DCreateHandle(&(arc->param));
		if (arc->ar3DHandle == NULL) {
			ARLOGe("setCamera(): Error creating 3D handle");
			return -1;
		}

		// ARLOGi("setCamera(): ar3DCreateHandle done\n");

		arPattAttach(arc->arhandle, arc->arPattHandle);
		// ARLOGi("setCamera(): Pattern handler attached.\n");

		arglCameraFrustum(&((arc->paramLT)->param), arc->nearPlane, arc->farPlane, arc->cameraLens);

		arc->kpmHandle = createKpmHandle(arc->paramLT);

		return 0;
	}




	/*****************
	* Marker loading *
	*****************/


	static int loadMarker(const char *patt_name, int *patt_id, ARHandle *arhandle, ARPattHandle **pattHandle_p) {
		// Loading only 1 pattern in this example.
		if ((*patt_id = arPattLoad(*pattHandle_p, patt_name)) < 0) {
			ARLOGe("loadMarker(): Error loading pattern file %s.\n", patt_name);
			arPattDeleteHandle(*pattHandle_p);
			return (FALSE);
		}

		return (TRUE);
	}

	static int loadMultiMarker(const char *patt_name, ARHandle *arHandle, ARPattHandle **pattHandle_p, ARMultiMarkerInfoT **arMultiConfig) {
		if( (*arMultiConfig = arMultiReadConfigFile(patt_name, *pattHandle_p)) == NULL ) {
			ARLOGe("config data load error !!\n");
			arPattDeleteHandle(*pattHandle_p);
			return (FALSE);
		}
		if( (*arMultiConfig)->patt_type == AR_MULTI_PATTERN_DETECTION_MODE_TEMPLATE ) {
			arSetPatternDetectionMode( arHandle, AR_TEMPLATE_MATCHING_COLOR );
		} else if( (*arMultiConfig)->patt_type == AR_MULTI_PATTERN_DETECTION_MODE_MATRIX ) {
			arSetPatternDetectionMode( arHandle, AR_MATRIX_CODE_DETECTION );
		} else { // AR_MULTI_PATTERN_DETECTION_MODE_TEMPLATE_AND_MATRIX
			arSetPatternDetectionMode( arHandle, AR_TEMPLATE_MATCHING_COLOR_AND_MATRIX );
		}

		return (TRUE);
	}


	int addMarker(int id, std::string patt_name) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		// const char *patt_name
		// Load marker(s).
		if (!loadMarker(patt_name.c_str(), &(arc->patt_id), arc->arhandle, &(arc->arPattHandle))) {
			ARLOGe("ARToolKitJS(): Unable to set up AR marker.\n");
			return -1;
		}

		return arc->patt_id;
	}

	int addNFTMarker(int id, std::string datasetPathname) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		// Load marker(s).
		int patt_id = arc->surfaceSetCount;
		if (!loadNFTMarker(arc, patt_id, datasetPathname.c_str())) {
			ARLOGe("ARToolKitJS(): Unable to set up NFT marker.\n");
			return -1;
		}

		arc->surfaceSetCount++;

		return patt_id;
	}

	int addMultiMarker(int id, std::string patt_name) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		// const char *patt_name
		// Load marker(s).
		if (!loadMultiMarker(patt_name.c_str(), arc->arhandle, &(arc->arPattHandle), &(arc->arMultiMarkerHandle))) {
			ARLOGe("ARToolKitJS(): Unable to set up AR multimarker.\n");
			return -1;
		}

		int multiMarker_id = arc->multi_markers.size();
		multi_marker marker = multi_marker();
		marker.id = multiMarker_id;
		marker.multiMarkerHandle = arc->arMultiMarkerHandle;

		arc->multi_markers.push_back(marker);

		return marker.id;
	}

	int getMultiMarkerNum(int id, int multiMarker_id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		int mId = multiMarker_id;
		if (mId < 0 || arc->multi_markers.size() <= mId) {
			return -1;
		}
		return (arc->multi_markers[mId].multiMarkerHandle)->marker_num;
	}

	int getMultiMarkerCount(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		return arc->multi_markers.size();
	}



	/**********************
	* Setters and getters *
	**********************/

	void setProjectionNearPlane(int id, const ARdouble projectionNearPlane) {
		if (arControllers.find(id) == arControllers.end()) { return; }
		arController *arc = &(arControllers[id]);
		arc->nearPlane = projectionNearPlane;
	}

	ARdouble getProjectionNearPlane(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);
		return arc->nearPlane;
	}

	void setProjectionFarPlane(int id, const ARdouble projectionFarPlane) {
		if (arControllers.find(id) == arControllers.end()) { return; }
		arController *arc = &(arControllers[id]);
		arc->farPlane = projectionFarPlane;
	}

	ARdouble getProjectionFarPlane(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);
		return arc->farPlane;
	}

	void setPatternDetectionMode(int id, int mode) {
		if (arControllers.find(id) == arControllers.end()) { return; }
		arController *arc = &(arControllers[id]);
		if (arSetPatternDetectionMode(arc->arhandle, mode) == 0) {
			ARLOGi("Pattern detection mode set to %d.\n", mode);
		}
	}

	int getPatternDetectionMode(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		int mode;
		arController *arc = &(arControllers[id]);
		if (arGetPatternDetectionMode(arc->arhandle, &mode) == 0) {
			return mode;
		}

		return -1;
	}

	void setPattRatio(int id, float ratio) {
		if (arControllers.find(id) == arControllers.end()) { return; }
		arController *arc = &(arControllers[id]);

		if (ratio <= 0.0f || ratio >= 1.0f) return;
		ARdouble pattRatio = (ARdouble)ratio;
		if (arc->arhandle) {
			if (arSetPattRatio(arc->arhandle, pattRatio) == 0) {
				ARLOGi("Pattern ratio size set to %f.\n", pattRatio);
			}
		}
	}

	ARdouble getPattRatio(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		ARdouble pattRatio;
		if (arc->arhandle) {
			if (arGetPattRatio(arc->arhandle, &pattRatio) == 0) {
				return pattRatio;
			}
		}

		return -1;
	}

	void setMatrixCodeType(int id, int type) {
		if (arControllers.find(id) == arControllers.end()) { return; }
		arController *arc = &(arControllers[id]);

		AR_MATRIX_CODE_TYPE matrixType = (AR_MATRIX_CODE_TYPE)type;
		arSetMatrixCodeType(arc->arhandle, matrixType);
	}

	int getMatrixCodeType(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		AR_MATRIX_CODE_TYPE matrixType;
		arGetMatrixCodeType(arc->arhandle, &matrixType);
		return matrixType;
	}

	void setLabelingMode(int id, int mode) {
		if (arControllers.find(id) == arControllers.end()) { return; }
		arController *arc = &(arControllers[id]);

		int labelingMode = mode;

		if (arSetLabelingMode(arc->arhandle, labelingMode) == 0) {
			ARLOGi("Labeling mode set to %d\n", labelingMode);
		}
	}

	int getLabelingMode(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		int labelingMode;

		if (arGetLabelingMode(arc->arhandle, &labelingMode) == 0) {
			return labelingMode;
		}

		return -1;
	}

	void setThreshold(int id, int threshold) {
		if (arControllers.find(id) == arControllers.end()) { return; }
		arController *arc = &(arControllers[id]);

		if (threshold < 0 || threshold > 255) return;
		if (arSetLabelingThresh(arc->arhandle, threshold) == 0) {
			ARLOGi("Threshold set to %d\n", threshold);
		};
		// default 100
		// arSetLabelingThreshMode
		// AR_LABELING_THRESH_MODE_MANUAL, AR_LABELING_THRESH_MODE_AUTO_MEDIAN, AR_LABELING_THRESH_MODE_AUTO_OTSU, AR_LABELING_THRESH_MODE_AUTO_ADAPTIVE
	}

	int getThreshold(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		int threshold;
		if (arGetLabelingThresh(arc->arhandle, &threshold) == 0) {
			return threshold;
		};

		return -1;
	}

	void setThresholdMode(int id, int mode) {
		if (arControllers.find(id) == arControllers.end()) { return; }
		arController *arc = &(arControllers[id]);

		AR_LABELING_THRESH_MODE thresholdMode = (AR_LABELING_THRESH_MODE)mode;

		if (arSetLabelingThreshMode(arc->arhandle, thresholdMode) == 0) {
			ARLOGi("Threshold mode set to %d\n", (int)thresholdMode);
		}
	}

	int getThresholdMode(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		AR_LABELING_THRESH_MODE thresholdMode;

		if (arGetLabelingThreshMode(arc->arhandle, &thresholdMode) == 0) {
			return thresholdMode;
		}

		return -1;
	}

	int setDebugMode(int id, int enable) {
		if (arControllers.find(id) == arControllers.end()) { return NULL; }
		arController *arc = &(arControllers[id]);

		arSetDebugMode(arc->arhandle, enable ? AR_DEBUG_ENABLE : AR_DEBUG_DISABLE);
		ARLOGi("Debug mode set to %s\n", enable ? "on." : "off.");

		return enable;
	}

	int getProcessingImage(int id) {
		if (arControllers.find(id) == arControllers.end()) { return NULL; }
		arController *arc = &(arControllers[id]);

		return (int)arc->arhandle->labelInfo.bwImage;
	}

	int getDebugMode(int id) {
		if (arControllers.find(id) == arControllers.end()) { return NULL; }
		arController *arc = &(arControllers[id]);

		int enable;

		arGetDebugMode(arc->arhandle, &enable);
		return enable;
	}

	void setImageProcMode(int id, int mode) {
		if (arControllers.find(id) == arControllers.end()) { return; }
		arController *arc = &(arControllers[id]);

		int imageProcMode = mode;
		if (arSetImageProcMode(arc->arhandle, mode) == 0) {
			ARLOGi("Image proc. mode set to %d.\n", imageProcMode);
		}
	}

	int getImageProcMode(int id) {
		if (arControllers.find(id) == arControllers.end()) { return -1; }
		arController *arc = &(arControllers[id]);

		int imageProcMode;
		if (arGetImageProcMode(arc->arhandle, &imageProcMode) == 0) {
			return imageProcMode;
		}

		return -1;
	}




	/*
	 * Marker processing
	 */

	void matrixCopy(ARdouble src[3][4], ARdouble dst[3][4]) {
		for (int i=0; i<3; i++) {
			for (int j=0; j<4; j++) {
				dst[i][j] = src[i][j];
			}
		}
	}

	int getTransMatSquare(int id, int markerIndex, int markerWidth) {
		if (arControllers.find(id) == arControllers.end()) { return ARCONTROLLER_NOT_FOUND; }
		arController *arc = &(arControllers[id]);

		if (arc->arhandle->marker_num <= markerIndex) {
			return MARKER_INDEX_OUT_OF_BOUNDS;
		}
		ARMarkerInfo* marker = markerIndex < 0 ? &gMarkerInfo : &((arc->arhandle)->markerInfo[markerIndex]);

		arGetTransMatSquare(arc->ar3DHandle, marker, markerWidth, gTransform);

		return 0;
	}

	int getTransMatSquareCont(int id, int markerIndex, int markerWidth) {
		if (arControllers.find(id) == arControllers.end()) { return ARCONTROLLER_NOT_FOUND; }
		arController *arc = &(arControllers[id]);

		if (arc->arhandle->marker_num <= markerIndex) {
			return MARKER_INDEX_OUT_OF_BOUNDS;
		}
		ARMarkerInfo* marker = markerIndex < 0 ? &gMarkerInfo : &((arc->arhandle)->markerInfo[markerIndex]);

		arGetTransMatSquareCont(arc->ar3DHandle, marker, gTransform, markerWidth, gTransform);

		return 0;
	}

	int setMarkerInfoDir(int id, int markerIndex, int dir) {
		if (arControllers.find(id) == arControllers.end()) { return ARCONTROLLER_NOT_FOUND; }
		arController *arc = &(arControllers[id]);

		if (arc->arhandle->marker_num <= markerIndex) {
			return MARKER_INDEX_OUT_OF_BOUNDS;
		}
		ARMarkerInfo* marker = markerIndex < 0 ? &gMarkerInfo : &((arc->arhandle)->markerInfo[markerIndex]);

		marker->dir = dir;

		return 0;
	}

	int setMarkerInfoVertex(int id, int markerIndex) {
		if (arControllers.find(id) == arControllers.end()) { return ARCONTROLLER_NOT_FOUND; }
		arController *arc = &(arControllers[id]);

		if (arc->arhandle->marker_num <= markerIndex) {
			return MARKER_INDEX_OUT_OF_BOUNDS;
		}
		ARMarkerInfo* marker = markerIndex < 0 ? &gMarkerInfo : &((arc->arhandle)->markerInfo[markerIndex]);

		auto v = marker->vertex;

		v[0][0] = gTransform[0][0];
		v[0][1] = gTransform[0][1];
		v[1][0] = gTransform[0][2];
		v[1][1] = gTransform[0][3];
		v[2][0] = gTransform[1][0];
		v[2][1] = gTransform[1][1];
		v[3][0] = gTransform[1][2];
		v[3][1] = gTransform[1][3];

		marker->pos[0] = (v[0][0] + v[1][0] + v[2][0] + v[3][0]) * 0.25;
		marker->pos[1] = (v[0][1] + v[1][1] + v[2][1] + v[3][1]) * 0.25;

		return 0;
	}

	int getTransMatMultiSquareRobust(int id, int multiMarkerId) {
		if (arControllers.find(id) == arControllers.end()) { return ARCONTROLLER_NOT_FOUND; }
		arController *arc = &(arControllers[id]);

		if (arc->multi_markers.size() <= multiMarkerId || multiMarkerId < 0) {
			return MULTIMARKER_NOT_FOUND;
		}
		multi_marker *multiMatch = &(arc->multi_markers[multiMarkerId]);
		ARMultiMarkerInfoT *arMulti = multiMatch->multiMarkerHandle;

		arGetTransMatMultiSquareRobust( arc->ar3DHandle, arc->arhandle->markerInfo, arc->arhandle->marker_num, arMulti );
		matrixCopy(arMulti->trans, gTransform);

		return 0;
	}

	int getTransMatMultiSquare(int id, int multiMarkerId) {
		if (arControllers.find(id) == arControllers.end()) { return ARCONTROLLER_NOT_FOUND; }
		arController *arc = &(arControllers[id]);

		if (arc->multi_markers.size() <= multiMarkerId || multiMarkerId < 0) {
			return MULTIMARKER_NOT_FOUND;
		}
		multi_marker *multiMatch = &(arc->multi_markers[multiMarkerId]);
		ARMultiMarkerInfoT *arMulti = multiMatch->multiMarkerHandle;

		arGetTransMatMultiSquare( arc->ar3DHandle, arc->arhandle->markerInfo, arc->arhandle->marker_num, arMulti );
		matrixCopy(arMulti->trans, gTransform);

		return 0;
	}

	int detectMarker(int id) {
		if (arControllers.find(id) == arControllers.end()) { return ARCONTROLLER_NOT_FOUND; }
		arController *arc = &(arControllers[id]);

		return arDetectMarker( arc->arhandle, arc->videoFrame );
	}

	int getMarkerNum(int id) {
		if (arControllers.find(id) == arControllers.end()) { return ARCONTROLLER_NOT_FOUND; }
		arController *arc = &(arControllers[id]);

		return arc->arhandle->marker_num;
	}

	int getMultiEachMarkerInfo(int id, int multiMarkerId, int markerIndex) {
		if (arControllers.find(id) == arControllers.end()) { return ARCONTROLLER_NOT_FOUND; }
		arController *arc = &(arControllers[id]);

		if (arc->multi_markers.size() <= multiMarkerId || multiMarkerId < 0) {
			return MULTIMARKER_NOT_FOUND;
		}
		multi_marker *multiMatch = &(arc->multi_markers[multiMarkerId]);
		ARMultiMarkerInfoT *arMulti = multiMatch->multiMarkerHandle;

		if (arMulti->marker_num <= markerIndex || markerIndex < 0) {
			return MARKER_INDEX_OUT_OF_BOUNDS;
		}

		ARMultiEachMarkerInfoT *marker = &(arMulti->marker[markerIndex]);
		matrixCopy(marker->trans, gTransform);

		EM_ASM_({
			if (!artoolkit["multiEachMarkerInfo"]) {
				artoolkit["multiEachMarkerInfo"] = ({});
			}
			var multiEachMarker = artoolkit["multiEachMarkerInfo"];
			multiEachMarker['visible'] = $0;
			multiEachMarker['pattId'] = $1;
			multiEachMarker['pattType'] = $2;
			multiEachMarker['width'] = $3;
		},
			marker->visible,
			marker->patt_id,
			marker->patt_type,
			marker->width
		);

		return 0;
	}

	int getMarkerInfo(int id, int markerIndex) {
		if (arControllers.find(id) == arControllers.end()) { return ARCONTROLLER_NOT_FOUND; }
		arController *arc = &(arControllers[id]);

		if (arc->arhandle->marker_num <= markerIndex) {
			return MARKER_INDEX_OUT_OF_BOUNDS;
		}
		ARMarkerInfo* markerInfo = markerIndex < 0 ? &gMarkerInfo : &((arc->arhandle)->markerInfo[markerIndex]);

		EM_ASM_({
			var $a = arguments;
			var i = 12;
			if (!artoolkit["markerInfo"]) {
				artoolkit["markerInfo"] = ({
					pos: [0,0],
					line: [[0,0,0], [0,0,0], [0,0,0], [0,0,0]],
					vertex: [[0,0], [0,0], [0,0], [0,0]]
				});
			}
			var markerInfo = artoolkit["markerInfo"];
			markerInfo["area"] = $0;
			markerInfo["id"] = $1;
			markerInfo["idPatt"] = $2;
			markerInfo["idMatrix"] = $3;
			markerInfo["dir"] = $4;
			markerInfo["dirPatt"] = $5;
			markerInfo["dirMatrix"] = $6;
			markerInfo["cf"] = $7;
			markerInfo["cfPatt"] = $8;
			markerInfo["cfMatrix"] = $9;
			markerInfo["pos"][0] = $10;
			markerInfo["pos"][1] = $11;
			markerInfo["line"][0][0] = $a[i++];
			markerInfo["line"][0][1] = $a[i++];
			markerInfo["line"][0][2] = $a[i++];
			markerInfo["line"][1][0] = $a[i++];
			markerInfo["line"][1][1] = $a[i++];
			markerInfo["line"][1][2] = $a[i++];
			markerInfo["line"][2][0] = $a[i++];
			markerInfo["line"][2][1] = $a[i++];
			markerInfo["line"][2][2] = $a[i++];
			markerInfo["line"][3][0] = $a[i++];
			markerInfo["line"][3][1] = $a[i++];
			markerInfo["line"][3][2] = $a[i++];
			markerInfo["vertex"][0][0] = $a[i++];
			markerInfo["vertex"][0][1] = $a[i++];
			markerInfo["vertex"][1][0] = $a[i++];
			markerInfo["vertex"][1][1] = $a[i++];
			markerInfo["vertex"][2][0] = $a[i++];
			markerInfo["vertex"][2][1] = $a[i++];
			markerInfo["vertex"][3][0] = $a[i++];
			markerInfo["vertex"][3][1] = $a[i++];
			markerInfo["errorCorrected"] = $a[i++];
			// markerInfo["globalID"] = $a[i++];
		},
			markerInfo->area,
			markerInfo->id,
			markerInfo->idPatt,
			markerInfo->idMatrix,
			markerInfo->dir,
			markerInfo->dirPatt,
			markerInfo->dirMatrix,
			markerInfo->cf,
			markerInfo->cfPatt,
			markerInfo->cfMatrix,

			markerInfo->pos[0],
			markerInfo->pos[1],

			markerInfo->line[0][0],
			markerInfo->line[0][1],
			markerInfo->line[0][2],

			markerInfo->line[1][0],
			markerInfo->line[1][1],
			markerInfo->line[1][2],

			markerInfo->line[2][0],
			markerInfo->line[2][1],
			markerInfo->line[2][2],

			markerInfo->line[3][0],
			markerInfo->line[3][1],
			markerInfo->line[3][2],

			//

			markerInfo->vertex[0][0],
			markerInfo->vertex[0][1],

			markerInfo->vertex[1][0],
			markerInfo->vertex[1][1],

			markerInfo->vertex[2][0],
			markerInfo->vertex[2][1],

			markerInfo->vertex[3][0],
			markerInfo->vertex[3][1],

			//

			markerInfo->errorCorrected

			// markerInfo->globalID
		);

		return 0;
	}


	/********
	* Setup *
	********/

	int setup(int width, int height, int cameraID) {
		int id = gARControllerID++;
		arController *arc = &(arControllers[id]);
		arc->id = id;

		arc->width = width;
		arc->height = height;

		arc->videoFrameSize = width * height * 4 * sizeof(ARUint8);
		arc->videoFrame = (ARUint8*) malloc(arc->videoFrameSize);

		if ((arc->arPattHandle = arPattCreateHandle()) == NULL) {
			ARLOGe("setup(): Error: arPattCreateHandle.\n");
		}

		setCamera(id, cameraID);

		ARLOGi("Allocated videoFrameSize %d\n", arc->videoFrameSize);

		EM_ASM_({
			if (!artoolkit["frameMalloc"]) {
				artoolkit["frameMalloc"] = ({});
			}
			var frameMalloc = artoolkit["frameMalloc"];
			frameMalloc["framepointer"] = $1;
			frameMalloc["framesize"] = $2;
			frameMalloc["camera"] = $3;
			frameMalloc["transform"] = $4;
		},
			arc->id,
			arc->videoFrame,
			arc->videoFrameSize,
			arc->cameraLens,
			gTransform
		);

		return arc->id;
	}



}

#include "ARBindEM.cpp"
