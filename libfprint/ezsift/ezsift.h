/* Copyright (c) 2013, Robert Wang, email: robertwgh (at) gmail.com
   All rights reserved. https://github.com/robertwgh/ezSIFT

   Some algorithms used in this code referred to:
   1. OpenCV: http://opencv.org/
   2. VLFeat: http://www.vlfeat.org/

   The SIFT algorithm was developed by David Lowe. More information can be found
   from: David G. Lowe, "Distinctive image features from scale-invariant
   keypoints," International Journal of Computer Vision, 60, 2 (2004), pp.
   91-110.

   Pay attention that the SIFT algorithm is patented. It is your responsibility
   to use the code in a legal way. Patent information: Method and apparatus for
   identifying scale invariant features in an image and use of same for locating
   an object in an image David G. Lowe, US Patent 6,711,293 (March 23, 2004).
   Provisional application filed March 8, 1999. Asignee: The University of
   British Columbia.

   Revision history:
      September, 15, 2013: initial version.
      July 8th, 2014, re-organized source code.
*/

#ifndef EZSIFT_H
#define EZSIFT_H

#include "image.h"
#include <list>
#include <vector>

namespace ezsift {

/****************************************
 * Constant parameters
 ***************************************/

// default number of sampled intervals per octave
extern int SIFT_INTVLS;

// default sigma for initial gaussian smoothing
extern float SIFT_SIGMA;

// the radius of Gaussian filter kernel;
// Gaussian filter mask will be (2*radius+1)x(2*radius+1).
// People use 2 or 3 most.
extern float SIFT_GAUSSIAN_FILTER_RADIUS;

// default threshold on keypoint contrast |D(x)|
extern float SIFT_CONTR_THR;

// default threshold on keypoint ratio of principle curvatures
extern float SIFT_CURV_THR;

// The keypoint refinement smaller than this threshold will be discarded.
extern float SIFT_KEYPOINT_SUBPiXEL_THR;

// double image size before pyramid construction?
extern bool SIFT_IMG_DBL;

// assumed gaussian blur for input image
extern float SIFT_INIT_SIGMA;

// width of border in which to ignore keypoints
extern int SIFT_IMG_BORDER;

// maximum steps of keypoint interpolation before failure
extern int SIFT_MAX_INTERP_STEPS;

// default number of bins in histogram for orientation assignment
extern int SIFT_ORI_HIST_BINS;

// determines gaussian sigma for orientation assignment
extern float SIFT_ORI_SIG_FCTR;

// determines the radius of the region used in orientation assignment
extern float SIFT_ORI_RADIUS;

// orientation magnitude relative to max that results in new feature
extern float SIFT_ORI_PEAK_RATIO;

// maximum number of orientations for each keypoint location
// static const float SIFT_ORI_MAX_ORI = 4;

// determines the size of a single descriptor orientation histogram
extern float SIFT_DESCR_SCL_FCTR;

// threshold on magnitude of elements of descriptor vector
extern float SIFT_DESCR_MAG_THR;

// factor used to convert floating-point descriptor to unsigned char
extern float SIFT_INT_DESCR_FCTR;

// default width of descriptor histogram array
extern int SIFT_DESCR_WIDTH;

// default number of bins per histogram in descriptor array
extern int SIFT_DESCR_HIST_BINS;

// default value of the nearest-neighbour distance ratio threshold
// |DR_nearest|/|DR_2nd_nearest|<SIFT_MATCH_NNDR_THR is considered as a match.
extern float SIFT_MATCH_NNDR_THR;

#if 0
// intermediate type used for DoG pyramids
typedef short sift_wt;
static const int SIFT_FIXPT_SCALE = 48;
#else
// intermediate type used for DoG pyramids
typedef float sift_wt;
static const int SIFT_FIXPT_SCALE = 1;
#endif

/****************************************
 * Definitions
 ***************************************/
#define DEGREE_OF_DESCRIPTORS (128)
struct SiftKeypoint {
    int octave;   // octave number
    int layer;    // layer number
    float rlayer; // real number of layer number

    float r;     // normalized row coordinate
    float c;     // normalized col coordinate
    float scale; // normalized scale

    float ri;          // row coordinate in that layer.
    float ci;          // column coordinate in that layer.
    float layer_scale; // the scale of that layer

    float ori; // orientation in degrees.
    float mag; // magnitude

    float descriptors[DEGREE_OF_DESCRIPTORS];
};

// Match pair structure. Use for interest point matching.
struct MatchPair {
    int r1;
    int c1;
    int r2;
    int c2;
};

/****************************************
 *  SIFT Processing Functions
 ***************************************/
// Initialize SIFT parameters.
void init_sift_parameters(bool doubleFirstOctave = true,
                          float contrast_threshold = 8.0f,
                          float edge_threshold = 10.0f,
                          float match_NDDR_threshold = 0.6f);

// Enable doubling of original image.
void double_original_image(bool doubleFirstOctave);

// Efficient Gaussian Blur function.
// 1. Use row buf to handle border pixel.
// 2. hori processing and transpose
int gaussian_blur(const Image<float> &in_image, Image<float> &out_image,
                  std::vector<float> coef1d);

// Row filter and then transpose
int row_filter_transpose(float *src, float *dst, int w, int h, float *coef1d,
                         int gR);

// Build image octaves during the initialization.
int build_octaves(const Image<unsigned char> &image,
                  std::vector<Image<unsigned char>> &octaves, int firstOctave,
                  int nOctaves);

// Compute Gaussian filter coefficients for Gaussian Blur.
std::vector<std::vector<float>> compute_gaussian_coefs(int nOctaves,
                                                       int nGpyrLayers);

// Build Gaussian pyramid.
int build_gaussian_pyramid(std::vector<Image<unsigned char>> &octaves,
                           std::vector<Image<float>> &gpyr, int nOctaves,
                           int nGpyrLayers);

// Build DoG pyramid.
int build_dog_pyr(std::vector<Image<float>> &gpyr,
                  std::vector<Image<float>> &dogPyr, int nOctaves,
                  int nDogLayers);

// Build gradient and rotation pyramid.
int build_grd_rot_pyr(std::vector<Image<float>> &gpyr,
                      std::vector<Image<float>> &grdPyr,
                      std::vector<Image<float>> &rotPyr, int nOctaves,
                      int nLayers);

// Refine local extrema.
bool refine_local_extrema(std::vector<Image<float>> &dogPyr, int nOctaves,
                          int nDogLayers, SiftKeypoint &kpt);

// Export keypoint list to a file.
int export_kpt_list_to_file(const char *filename,
                            std::list<SiftKeypoint> &kpt_list,
                            bool bIncludeDescpritor);

// Compute orientation histogram.
float compute_orientation_hist(const Image<float> &image, SiftKeypoint &kpt,
                               float *&hist);

/****************************************
 *  SIFT Core Functions
 ***************************************/
// Detect keypoints.
int detect_keypoints(std::vector<Image<float>> &dogPyr,
                     std::vector<Image<float>> &grdPyr,
                     std::vector<Image<float>> &rotPyr, int nOctaves,
                     int nDogLayers, std::list<SiftKeypoint> &kpt_list);

// Extract descriptor.
int extract_descriptor(std::vector<Image<float>> &grdPyr,
                       std::vector<Image<float>> &rotPyr, int nOctaves,
                       int nGpyrLayers, std::list<SiftKeypoint> &kpt_list);

/****************************************
 *  SIFT Interface Functions
 ***************************************/
// Detect keypoints and extract descriptor.
int sift_cpu(const Image<unsigned char> &image,
             std::list<SiftKeypoint> &kpt_list, bool bExtractDescriptors);

// Match keypoints from two keypoint lists.
int match_keypoints(std::list<SiftKeypoint> &kpt_list1,
                    std::list<SiftKeypoint> &kpt_list2,
                    std::list<MatchPair> &match_list);

} // end namespace ezsift

#endif
