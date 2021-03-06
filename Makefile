# apt packages required include libopencv-dev libboost-dev libboost-thread-dev libboost-program-options-dev libboost-regex-dev libboost-filesystem-dev libboost-program-options-dev
#CXXFLAGS=-ggdb -O3 -DNDEBUG
CXXFLAGS=-ggdb
LDLIBS=-lopencv_core -lopencv_imgproc -lopencv_highgui -lopencv_ml -lopencv_video -lopencv_features2d -lopencv_calib3d -lopencv_objdetect -lopencv_contrib -lopencv_legacy -lopencv_flann -lboost_thread -lboost_regex -lboost_system -lboost_program_options -lboost_filesystem -lboost_date_time -lopencv_calib3d

SRCS=main.cpp demosaic.cpp prototype.cpp illumination.cpp stereo.cpp interpolation.cpp learn_correct.cpp logging.cpp utils.cpp AltitudeFromStereo.cpp RectifyImage.cpp Reconstruct3dImage.cpp DataIO.cpp
OBJS=$(subst .cpp,.o,$(SRCS))

EXE=bic

includes = $(wildcard *.hpp)

all: $(EXE)

bic: $(OBJS)
	g++ $(LDFLAGS) -o $(EXE) $(OBJS) $(LDLIBS) 

%.o: %.cpp ${includes}
	g++ $(CXXFLAGS) -c $< -o $@

clean:
	$(RM) $(OBJS)
	$(RM) $(EXE)

