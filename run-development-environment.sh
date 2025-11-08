docker run -d --privileged --network=host -it --rm --user root \
  --memory=32g --memory-swap=64g \
  --name chipyard-development-environment \
  -v /tmp/.X11-unix:/tmp/.X11-unix \
  -v /home/ac/CodingWorkspace/hardware/chipyard:/external-workspace/chipyard \
  -v /home/ac/CodingWorkspace/hardware/hammer:/external-workspace/hammer \
  eyeamansh/chipyard-dev:latest
