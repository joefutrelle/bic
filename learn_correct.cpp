#include <iostream>
#include <string>
#include <fstream>
#include <ios>
#include <opencv2/opencv.hpp>
#include <boost/thread.hpp>
#include <boost/format.hpp>
#include <boost/asio.hpp>
#include <boost/tokenizer.hpp>

#include "learn_correct.hpp"
#include "prototype.hpp"
#include "demosaic.hpp"
#include "illumination.hpp"
#include "interpolation.hpp"

using namespace std;
using namespace cv;

using illum::MultiLightfield;

// this is a prototype application; code is not in reusable state yet

#define PATH_FILE "aprs.csv" // FIXME hardcoded CSV file path
#define BAYER_PATTERN "rggb" // FIXME hardcoded bayer pattern
#define OUT_DIR "out" // FIXME hardcoded model/output directory
#define N_THREADS 12 // FIXME hardcoded thread count
#define ALT_SPACING_M 0.1 // FIXME hardcoded altitude bin spacing
#define FOCAL_LENGTH_M 0.012 // FIXME hardcoded focal length
#define PIXEL_SEP_M 0.0000065 // FIXME hardcoded pixel separation

// the learn task adds an image to a multilightfield model
void learn_task(MultiLightfield *model, string inpath, double alt, double pitch, double roll) {
  using boost::format;
  cerr << nounitbuf;
  // get the input pathname
  try  {
    cerr << format("POPPED %s %.2f,%.2f,%.2f") % inpath % alt % pitch % roll << endl;
    // read the image (this can be done in parallel)
    Mat cfa_LR = imread(inpath, CV_LOAD_IMAGE_ANYDEPTH);
    if(!cfa_LR.data)
      throw std::runtime_error("unable to read image file");
    if(cfa_LR.type() != CV_16U)
      throw std::runtime_error("image is not 16-bit grayscale");
    cerr << format("READ %s") % inpath << endl;
    model->addImage(cfa_LR, alt, pitch, roll);
    cerr << format("ADDED %s") % inpath << endl;
  } catch(std::runtime_error const &e) {
    cerr << "ERROR learning " << inpath << ": " << e.what() << endl;
  } catch(std::exception) {
    cerr << "ERROR learning " << inpath << endl;
  }
}

// the correct task corrects images
void correct_task(MultiLightfield *model, string inpath, double alt, double pitch, double roll, string outpath) {
  using boost::format;
  cerr << nounitbuf;
  try {
    cerr << format("POPPED %s %.2f,%.2f,%.2f") % inpath % alt % pitch % roll << endl;
    Mat cfa_LR = imread(inpath, CV_LOAD_IMAGE_ANYDEPTH); // read input image
    if(!cfa_LR.data)
      throw std::runtime_error("no image data");
    if(cfa_LR.type() != CV_16U)
      throw std::runtime_error("image is not 16-bit grayscale");
    // get the average
    Mat average = Mat::zeros(cfa_LR.size(), CV_32F);
    model->getAverage(average, alt, pitch, roll);
    // now smooth the average
    int h = average.size().height;
    int w = average.size().width;
    Mat left = Mat(average,Rect(0,0,w/2,h));
    Mat right = Mat(average,Rect(w/2,0,w/2,h));
    cfa_smooth(left,left,30); // FIXME hardcoded smoothing kernel size
    cfa_smooth(right,right,30);
    cerr << "SMOOTHED lightmap" << endl;
    illum::correct(cfa_LR, cfa_LR, average); // correct it
    cerr << format("DEMOSAICING %s") % inpath << endl;
    // demosaic it
    Mat rgb_LR = demosaic(cfa_LR,BAYER_PATTERN); // FIXME hardcoded param bayer pattern
    /*stringstream outpath_exts;
      string outpath_ext;
      outpath_exts << outpath << ".png";
      outpath_ext = outpath_exts.str();*/
    outpath = outpath + ".png";
    cerr << format("SAVE RGB to %s") % outpath << endl;
    double max = 0.7; // FIXME hardcoded max brightness
    double min = 0.05; // FIXME hardcoded min brightness
    // scale brightness and save as 8-bit png
    Mat rgb_LR_8u;
    rgb_LR = rgb_LR * (255.0 / (65535.0 * (max - min))) - (min * 255.0);
    rgb_LR.convertTo(rgb_LR_8u, CV_8U);
    imwrite(outpath, rgb_LR_8u);
  } catch(std::runtime_error const &e) {
    cerr << "ERROR correcting " << inpath << ": " << e.what() << endl;
  } catch(std::exception) {
    cerr << "ERROR correcting " << inpath << endl;
  }
}

void validate_params(double alt, double pitch_deg, double roll_deg) {
  using boost::format;
  if(alt < 0 || alt > 8) // FIXME hardcoded
    throw std::runtime_error(str(format("ERROR altitude out of range: %.2f") % alt));
  else if(pitch_deg < -45 || pitch_deg > 45) // FIXME hardcoded
    throw std::runtime_error(str(format("ERROR pitch out of range: %.2f") % pitch_deg));
  else if(roll_deg < -45 || roll_deg > 45) // FIXME hardcoded
    throw std::runtime_error(str(format("ERROR roll out of range: %.2f") % roll_deg));
}

// learn phase
void learn_correct::learn() {
  // before any OpenCV operations are done, set global error flag
  cv::setBreakOnError(true);
  // ersatz logging setup
  using boost::format;
  cerr << nounitbuf;
  // construct an empty lightfield model
  illum::MultiLightfield model(ALT_SPACING_M, FOCAL_LENGTH_M, PIXEL_SEP_M); // FIXME hardcoded altitude bin spacing and camera params
  // post all work
  boost::asio::io_service io_service;
  boost::thread_group workers;
  // start up the work threads
  // use the work object to keep threads alive before jobs are posted
  // use auto_ptr so we can indicate that no more jobs will be posted
  auto_ptr<boost::asio::io_service::work> work(new boost::asio::io_service::work(io_service));
  // create the thread pool
  for(int i = 0; i < N_THREADS; i++) { // FIXME hardcoded thread count
    workers.create_thread(boost::bind(&boost::asio::io_service::run, &io_service));
  }
  // now read input lines and post jobs
  ifstream inpaths(PATH_FILE); // FIXME hardcoded CSV file pathname
  string line;
  typedef boost::tokenizer< boost::escaped_list_separator<char> > Tokenizer;
  vector<string> fields;
  while(getline(inpaths,line)) { // read pathames from a file
    try {
      Tokenizer tok(line);
      fields.assign(tok.begin(),tok.end());
      // FIXME tile
      int f=0;
      string inpath = fields.at(f++);
      string outpath = fields.at(f++);
      double alt = atof(fields.at(f++).c_str()); // altitude in m
      double pitch_deg = atof(fields.at(f++).c_str());
      double roll_deg = atof(fields.at(f++).c_str());
      validate_params(alt, pitch_deg, roll_deg);
      double pitch = M_PI * pitch_deg / 180.0;
      double roll = M_PI * roll_deg / 180.0;
      io_service.post(boost::bind(learn_task, &model, inpath, alt, pitch, roll));
      cerr << format("PUSHED LEARN %s") % inpath << endl;
    } catch(std::runtime_error const &e) {
      cerr << format("ERROR parsing input metadata: %s") % e.what() << endl;
    } catch(std::exception) {
      cerr << "ERROR parsing input metadata" << endl;
    }
  }
  // destroy the work object to indicate that there are no more jobs
  work.reset();
  // now run all pending jobs to completion
  workers.join_all();
  cerr << "SUCCESS learn phase" << endl;

  model.save(OUT_DIR); // FIXME hardcoded model/output directory
  cerr << "SAVED model" << endl;
}

void learn_correct::correct() {
  // before any OpenCV operations are done, set global error flag
  cv::setBreakOnError(true);
  using boost::format;
  cerr << nounitbuf;
  // load model
  cerr << "LOADING model..." << endl;
  illum::MultiLightfield model;
  model.load(OUT_DIR); // FIXME hardcoded model/output directory
  cerr << "LOADED model" << endl;

  // post all work
  boost::asio::io_service io_service;
  // use the work object to keep threads alive before jobs are posted
  // use auto_ptr so we can indicate that no more jobs will be posted
  auto_ptr<boost::asio::io_service::work> work(new boost::asio::io_service::work(io_service));
  // start a thread pool
  boost::thread_group workers;
  for(int i = 0; i < N_THREADS; i++) { // FIXME hardcoded thread count
    workers.create_thread(boost::bind(&boost::asio::io_service::run, &io_service));
  }
  // post jobs
  ifstream inpaths(PATH_FILE);  // FIXME hardcoded CSV file pathname
  string line;
  typedef boost::tokenizer< boost::escaped_list_separator<char> > Tokenizer;
  vector<string> fields;
  while(getline(inpaths,line)) { // read pathames from a file
    try {
      Tokenizer tok(line);
      fields.assign(tok.begin(),tok.end());
      int f=0;
      string inpath = fields.at(f++);
      string outpath = fields.at(f++);
      double alt = atof(fields.at(f++).c_str()); // altitude in m
      double pitch_deg = atof(fields.at(f++).c_str());
      double roll_deg = atof(fields.at(f++).c_str());
      validate_params(alt, pitch_deg, roll_deg);
      double pitch = M_PI * pitch_deg / 180.0;
      double roll = M_PI * roll_deg / 180.0;
      io_service.post(boost::bind(correct_task, &model, inpath, alt, pitch, roll, outpath));
      cerr << format("PUSHED CORRECT %s") % inpath << endl;
    } catch(std::runtime_error const &e) {
      cerr << format("ERROR parsing input metadata: %s") % e.what() << endl;
    } catch(std::exception) {
      cerr << "ERROR parsing input metadata" << endl;
    }
  }
  // destroy the work object to indicate that there are no more jobs
  work.reset();
  // now run all pending jobs to completion
  workers.join_all();
  // we're done
  cerr << "SUCCESS correct phase" << endl;
}