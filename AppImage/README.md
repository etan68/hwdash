# Build the AppImage

```bash
podman pull ubuntu:18.04
podman run --interactive --tty --rm --volume $PWD:/hwdash ubuntu:24.04
cd hwdash
./AppImage/make_appimage.sh
```
