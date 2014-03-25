#include <iostream>
#include <string>
#include <fstream>
#include <ios>
#include <boost/thread.hpp>
#include <boost/format.hpp>
#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>
#include <opencv2/opencv.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include "learn_correct.hpp"
#include "stereo.hpp"
#include "prototype.hpp"
#include "demosaic.hpp"
#include "illumination.hpp"
#include "interpolation.hpp"
#include "logging.hpp"

using std::string;

namespace fs = boost::filesystem;

using illum::MultiLightfield;
using learn_correct::Params;
using jlog::log;
using jlog::log_error;

using boost::format;
using boost::str;

double compute_missing_alt(Params *params, double alt, cv::Mat cfa_LR, std::string inpath) {
  using stereo::align;
  using cv::Mat;
  // if altitude is good, don't recompute it
  if(!params->alt_from_parallax && alt > 0 && alt < MAX_ALTITUDE)
    return alt;
  // compute from parallax
  // pull green channel
  Mat G;
  if(params->bayer_pattern[0]=='g') {
    cfa_channel(cfa_LR, G, 0, 0);
  } else {
    cfa_channel(cfa_LR, G, 1, 0);
  }
  // compute pixel offset
  int x = align(G, params->parallax_template_size) * 2;
  if(x <= 0)
    throw std::runtime_error(str(format("unable to compute altitude from parallax for %s") % inpath));
  // convert to meters
  alt = (params->camera_sep * params->focal_length * H2O_ADJUSTMENT) / (x * params->pixel_sep);
  // log what just happened
  log("PARALLAX altitude of %s is %.2f") % inpath % alt;
  return alt;
}

// the learn task adds an image to a multilightfield model
void learn_task(Params *params, MultiLightfield *model, string inpath, double alt, double pitch, double roll, std::vector<std::string >* learned) {
  using cv::Mat;
  // get the input pathname
  try  {
    log("START LEARN %s %.2f,%.2f,%.2f") % inpath % alt % pitch % roll;
    // read the image (this can be done in parallel)
    Mat cfa_LR = cv::imread(inpath, CV_LOAD_IMAGE_ANYDEPTH);
    if(!cfa_LR.data)
      throw std::runtime_error(str(format("unable to read image file: %s") % inpath));
    if(cfa_LR.type() != CV_16U)
      throw std::runtime_error(str(format("image is not 16-bit grayscale: %s") % inpath));
    log("READ %s") % inpath;
    // if altitude is out of range, compute from parallax
    alt = compute_missing_alt(params, alt, cfa_LR, inpath);
    model->addImage(cfa_LR, alt, pitch, roll);
    learned->push_back(str(format("%s,%.2f,%.2f,%.2f") % inpath % alt % pitch % roll));
    log("LEARNED %s") % inpath;
  } catch(std::runtime_error const &e) {
    log_error("ERROR learning %s: %s") % inpath % e.what();
  } catch(std::exception) {
    log_error("ERROR learning %s") % inpath;
  }
}

// the correct task corrects images
void correct_task(Params *params, MultiLightfield *model, string inpath, double alt, double pitch, double roll, string outpath) {
  using cv::Mat;
  using boost::algorithm::ends_with;
  try {
    log("START CORRECT %s %.2f,%.2f,%.2f") % inpath % alt % pitch % roll;
    // make sure output path ends with ".png"
    string lop = outpath;
    boost::to_lower(lop);
    if(!ends_with(lop,".png"))
      outpath = outpath + ".png";
    // first, make sure we can write the output file
    fs::path outp(outpath);
    fs::path outdir = outp.parent_path();
    // now create output directory if necessary
    if(params->create_directories)
      fs::create_directories(outdir);
    // proceed
    Mat cfa_LR = cv::imread(inpath, CV_LOAD_IMAGE_ANYDEPTH); // read input image
    if(!cfa_LR.data)
      throw std::runtime_error("no image data");
    if(cfa_LR.type() != CV_16U)
      throw std::runtime_error("image is not 16-bit grayscale");
    // if altitude is out of range, compute from parallax
    alt = compute_missing_alt(params, alt, cfa_LR, inpath);
    // get the average
    Mat average = Mat::zeros(cfa_LR.size(), CV_32F);
    model->getAverage(average, alt, pitch, roll);
    // now smooth the average
    int h = average.size().height;
    int w = average.size().width;
    if(params->stereo) {
      Mat left = Mat(average,cv::Rect(0,0,w/2,h));
      Mat right = Mat(average,cv::Rect(w/2,0,w/2,h));
      cfa_smooth(left,left,params->lightmap_smoothing);
      cfa_smooth(right,right,params->lightmap_smoothing);
    } else {
      cfa_smooth(cfa_LR,cfa_LR,params->lightmap_smoothing);
    }
    log("SMOOTHED lightmap for %s") % inpath;
    illum::correct(cfa_LR, cfa_LR, average); // correct it
    log("DEMOSAICING %s") % inpath;
    // demosaic it
    Mat rgb_LR = demosaic(cfa_LR,params->bayer_pattern);
    // brightness and contrast parameters
    double max = params->max_brightness;
    double min = params->min_brightness;
    // adjust brightness/contrast and save as 8-bit png
    Mat rgb_LR_8u;
    rgb_LR = rgb_LR * (255.0 / (65535.0 * (max - min))) - (min * 255.0);
    rgb_LR.convertTo(rgb_LR_8u, CV_8U);
    // now write the output image
    log("SAVING corrected image to %s") % outpath;
    if(!imwrite(outpath, rgb_LR_8u))
      throw std::runtime_error(str(format("unable to write output image to %s") % outpath));
    log("CORRECTED %s") % inpath;
  } catch(std::runtime_error const &e) {
    log_error("ERROR correcting %s: %s") % inpath % e.what();
  } catch(std::exception) {
    log_error("ERROR correcting %s") % inpath;
  }
}

std::istream* learn_correct::get_input(learn_correct::Params p) {
  if(p.input == "-") { // use stdin
    return &std::cin;
  } else {
    return new std::ifstream(p.input.c_str());
  }
}

void write_paramfile(Params p, fs::path paramfile) {
  // write Params to param file
  log("WRITING paramfile %s") % paramfile;
  std::ofstream pout(paramfile.string().c_str());
  if(!(pout << p)) // write parameters to parameter file
    throw std::runtime_error(str(format("failed to write parameter file to %s") % paramfile.c_str()));
  pout.close();
  log("WROTE paramfile %s") % paramfile;
}

void write_lightmap(MultiLightfield* model, fs::path outdir) {
  log("SAVING lightmap in %s ...") % outdir;
  model->save(outdir.string());
  log("SAVED lightmap in %s") % outdir;
}

// read/write "learned" CSV data and add to a set of RAW image pathnames
// that are in the lightmap and so should be skipped if they are
// encountered again
void write_skipfile(fs::path skipfile, std::vector<string >* learned) {
  // write "learned" CSV data to skip file
  log("WRITING skipfile %s") % skipfile;
  std::ofstream sout(skipfile.string().c_str());
  for(std::vector<std::string>::const_iterator i = learned->begin(); i != learned->end(); ++i) {
    sout << *i << std::endl;
  }
  sout.close();
  log("WROTE skipfile %s") % skipfile;
}

void read_skipfile(fs::path skipfile, std::vector<string >* learned, std::set<string >* skip) {
  typedef boost::tokenizer< boost::escaped_list_separator<char> > Tokenizer;
  log("READING skipfile %s") % skipfile;
  std::ifstream sin(skipfile.string().c_str());
  string line;
  while(getline(sin,line)) {
    learned->push_back(line); // push CSV record to learned
    Tokenizer tok(line);
    string infile = *tok.begin();
    skip->insert(infile); // filename is first item in CSV record
  }
  log("READ skipfile %s") % skipfile;
}

// checkpoint the current state of a learn process
void checkpoint(Params p, MultiLightfield* model, std::vector<std::string >* learned, string _outdir="") {
  // output directory (_outdir, if non-empty, overrides parameter)
  fs::path outdir(_outdir.empty() ? p.lightmap_dir : _outdir);
  // parameter file contains program options
  fs::path paramfile = outdir / "params.txt";
  // skipfile lists images in the lightmap along with alt / pitch / roll
  fs::path skipfile = outdir / "learned.csv";
  // make sure the directory exists
  if(p.create_directories)
    fs::create_directories(outdir);
  if(!fs::exists(outdir))
    throw std::runtime_error(str(format("lightmap directory %s does not exist") % outdir));
  // now write params
  write_paramfile(p, paramfile);
  // now write lightmap
  write_lightmap(model, outdir);
  // now write skipfile
  write_skipfile(skipfile, learned);
}

// resume from an existing lightmap
int resume(Params p, MultiLightfield *model, std::vector<string >* learned, std::set<string >* skip, string _outdir="") {
  // output directory (_outdir, if non-empty, overrides parameter)
  fs::path outdir(_outdir.empty() ? p.lightmap_dir : _outdir);
  // skipfile lists images in the lightmap along with alt / pitch / roll
  fs::path skipfile = outdir / "learned.csv";
  // FIXME read parameters from param file
  // read skipfile, if there is one
  if(fs::exists(skipfile))
    read_skipfile(skipfile, learned, skip);
  // now read lightmap
  log("LOADING lightmap from %s ...") % outdir;
  return model->load(outdir.string());
  log("LOADED lightmap from %s") % outdir;
}

void do_learn_correct(learn_correct::Params p, bool learn, bool correct) {
  using learn_correct::Task;
  // before any OpenCV operations are done, set global error flag
  cv::setBreakOnError(true);
  // paths to lightmap files
  fs::path outdir(p.lightmap_dir);
  // parameter file contains program options
  fs::path paramfile = outdir / "params.txt";
  // skipfile lists images in the lightmap along with alt / pitch / roll
  fs::path skipfile = outdir / "learned.csv";
  // construct an empty lightfield model based on parameters
  illum::MultiLightfield model(p.alt_spacing, p.focal_length, p.pixel_sep);
  // construct an initially empty skip list
  std::set<std::string > skip;
  // construct an initially empty "learned" list
  std::vector<std::string > learned;
  //
  if(learn && p.update) { // updating?
    // resume from existing checkpoint
    resume(p, &model, &learned, &skip);
  } else if(learn && !p.update) {
    // create an empty checkpoint
    checkpoint(p, &model, &learned);
  } else if(correct) {
    log("LOADING model from %s ...") % p.lightmap_dir;
    int loaded = model.load(p.lightmap_dir);
    if(!learn && !loaded)
      throw std::runtime_error(str(format("lightmap in %s is empty, cannot correct without training") % p.lightmap_dir));
    log("LOADED model from %s") % p.lightmap_dir;
  }
  // now do a chunk of work, checkpoint, and continue
  int n_todo = 0;
  while(n_todo <= 0) {
    // post all work
    boost::asio::io_service io_service;
    boost::thread_group workers;
    // start up the work threads
    // use the work object to keep threads alive before jobs are posted
    // use auto_ptr so we can indicate that no more jobs will be posted
    std::auto_ptr<boost::asio::io_service::work> work(new boost::asio::io_service::work(io_service));
    // create the thread pool
    for(int i = 0; i < p.n_threads; i++) {
      workers.create_thread(boost::bind(&boost::asio::io_service::run, &io_service));
    }
    // now read input lines and post jobs
    std::istream* csv_in = get_input(p);
    n_todo = p.batch_size; // how many images to process in this batch
    string line;
    int n_learned_then = learned.size();
    while(getline(*csv_in,line) && (!learn || n_todo--)) { // read pathames from a file
      try {
	// parse the input line and turn it into a Task object
	Task task = Task(line);
	// check that the task is valid
	task.validate();
	if(learn) { // if learning
	  // is the inpath on the skip list?
	  if(skip.count(task.inpath) == 0) {
	    // push a learn task on the queue
	    io_service.post(boost::bind(learn_task, &p, &model, task.inpath, task.alt, task.pitch, task.roll, &learned));
	    log("QUEUED LEARN %s") % task.inpath;
	  } else {
	    log("SKIPPING LEARN %s") % task.inpath;
	  }
	}
	if(correct) { // if correcting
	  // push a correct task on the queue
	  io_service.post(boost::bind(correct_task, &p, &model, task.inpath, task.alt, task.pitch, task.roll, task.outpath));
	  log("QUEUED CORRECT %s") % task.inpath;
	}
      } catch(std::runtime_error const &e) {
	log_error("ERROR parsing input metadata: %s: last line read was '%s'") % e.what() % line;
      } catch(std::exception) {
	log_error("ERROR parsing input metadata: last line read was '%s'") % line;
      }
    }
    // destroy the work object to indicate that there are no more jobs
    work.reset();
    // now run all pending jobs to completion
    workers.join_all();

    int n_learned_now = learned.size();
    // we know output directory already exists and can be written to
    if(learn && n_todo < p.batch_size && n_learned_now > n_learned_then) {
      checkpoint(p, &model, &learned);
    }
  }// move on to next chunk
  log("COMPLETE");
}

// learn phase
void learn_correct::learn(learn_correct::Params p) {
  do_learn_correct(p, true, false);
}

void learn_correct::correct(learn_correct::Params p) {
  do_learn_correct(p, false, true);
}

// adaptive (learn + correct)
void learn_correct::adaptive(learn_correct::Params p) {
  do_learn_correct(p, true, true);
}
