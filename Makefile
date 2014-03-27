##############################################################################
# Files
##############################################################################
APP_NAME       = simpleServerDNS
APP_OFILES    += Error.o
APP_OFILES    += main.o
APP_OFILES    += Packet.o
APP_OFILES    += Server.o

##############################################################################
# Settings
##############################################################################
COMPILER      = g++
DEBUG        ?= 3
FINAL        ?= 0
FLAGS        += -c
FLAGS        += -DENDIAN_LITTLE
FLAGS        += -std=c++11
ifneq ($(FINAL), 0)
FLAGS        += -O$(FINAL)
FLAGS        += -DNDEBUG
else
#FLAGS       += -ggdb$(DEBUG)
endif

##############################################################################
# Targets
##############################################################################
# (make) will make the standard app with debug symbols
# (make -DFINAL=3) will make the fully optmized release binary

$(APP_NAME): $(APP_OFILES)
	$(COMPILER) -o $(APP_NAME) $(APP_OFILES) $(LIBS)

clean:
	rm -rf $(APP_NAME) $(APP_OFILES)

##############################################################################
# Build Rules
##############################################################################
%.o : %.cpp
	$(COMPILER) -o $@ $(FLAGS) $<


