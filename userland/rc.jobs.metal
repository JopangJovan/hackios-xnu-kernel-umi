# Commands run by the launchd job com.redwood.demo (see com.redwood.demo.plist).
# What iOS here believes about Metal (RC_JOBS=rc.jobs.metal, a run of half a minute): the services Metal
# looks for and does not find, the GPU the registry does have under another name, whether a plug-in bundle
# of this image would load at all, the nil a program gets, what MobileGestalt still says about graphics,
# and whether CoreImage can run a filter kernel without Metal. See src/metal_test.m and docs/metal.md.
/redwood/bin/metal_test
