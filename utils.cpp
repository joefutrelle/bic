#include <string>
#include <opencv2/opencv.hpp>
#include <boost/thread.hpp>
#include <boost/asio.hpp>
#include <boost/tokenizer.hpp>
#include <boost/filesystem.hpp>

#include "utils.hpp"
#include "learn_correct.hpp"
#include "demosaic.hpp"
#include "stereo.hpp"
#include "logging.hpp"

namespace fs = boost::filesystem;

using namespace std;
using namespace cv;

using jlog::log;
using jlog::log_error;

using learn_correct::Params;
using stereo::CameraPair;

void alt_task(Params* params, string inpath) {
  try {
    Mat cfa_LR = imread(inpath, CV_LOAD_IMAGE_ANYDEPTH); // read input image
    if(!cfa_LR.data)
      throw std::runtime_error("no image data");
    if(cfa_LR.type() != CV_16U)
      throw std::runtime_error("image is not 16-bit grayscale");
    // pull green channel
    // pull green channel
    Mat G;
    if(params->bayer_pattern[0]=='g') {
      cfa_channel(cfa_LR, G, 0, 0);
    } else {
      cfa_channel(cfa_LR, G, 1, 0);
    }
    // compute pixel offset
    int x = stereo::align(G, params->parallax_template_size) * 2;
    // set up a camera pair
    CameraPair cameras(params->camera_sep, params->focal_length, params->pixel_sep);
    double alt = cameras.xoff2alt(x);
    log("%s,%.2f") % inpath % alt;
  } catch(std::runtime_error const &e) {
    log_error("ERROR %s: %s") % inpath % e.what();
  } catch(std::exception) {
    log_error("ERROR %s") % inpath;
  }
}

void utils::alt_from_stereo(Params params) {
  // before any OpenCV operations are done, set global error flag
  cv::setBreakOnError(true);
  // post all work
  boost::asio::io_service io_service;
  boost::thread_group workers;
  // start up the work threads
  // use the work object to keep threads alive before jobs are posted
  // use auto_ptr so we can indicate that no more jobs will be posted
  auto_ptr<boost::asio::io_service::work> work(new boost::asio::io_service::work(io_service));
  // create the thread pool
  for(int i = 0; i < params.n_threads; i++) {
    workers.create_thread(boost::bind(&boost::asio::io_service::run, &io_service));
  }
  // post jobs
  istream *csv_in = learn_correct::get_input(params);
  string line;
  while(getline(*csv_in,line)) { // read pathames from a file
    try {
      string inpath = line;
      if(fs::exists(inpath)) {
	io_service.post(boost::bind(alt_task, &params, inpath));
      } else {
	log_error("WARNING: can't find %s") % inpath;
      }
    } catch(std::runtime_error const &e) {
      log_error("ERROR parsing input metadata: %s") % e.what();
    } catch(std::exception) {
      log_error("ERROR parsing input metadata");
    }
  }
  // destroy the work object to indicate that there are no more jobs
  work.reset();
  // now run all pending jobs to completion
  workers.join_all();
}