package main

import (
	"os"

	gw "github.com/cvmfs/gateway/internal/gateway"
	be "github.com/cvmfs/gateway/internal/gateway/backend"
	fe "github.com/cvmfs/gateway/internal/gateway/frontend"
)

func main() {
	gw.InitLogging(os.Stderr)
	cfg, err := gw.ReadConfig()
	if err != nil {
		gw.Log("main", gw.LogError).
			Msg("reading configuration failed")
		os.Exit(1)
	}
	gw.ConfigLogging(cfg)

	gw.Log("main", gw.LogDebug).
		Msgf("configuration read: %+v", cfg)

	gw.Log("main", gw.LogInfo).
		Msg("starting repository gateway")

	services, err := be.StartBackend(cfg)
	if err != nil {
		gw.Log("main", gw.LogError).
			Err(err).
			Msg("could not start backend services")
		os.Exit(1)
	}
	defer services.Stop()

	timeout := services.Config.MaxLeaseTime
	if err := fe.Start(services, cfg.Port, timeout); err != nil {
		gw.Log("main", gw.LogError).
			Err(err).
			Err(err).
			Msg("starting the HTTP front-end failed")
		os.Exit(1)
	}
}
