{
  description = "Guitar DI to MIDI, real time, on Linux";

  inputs = {
    nixpkgs.url = "https://channels.nixos.org/nixpkgs-unstable/nixexprs.tar.zst";
  };

  outputs = inputs: let
    systems = ["x86_64-linux" "aarch64-linux"];
    forAllSystems = f:
      builtins.listToAttrs (map (system: {
          name = system;
          value = f system inputs.nixpkgs.legacyPackages.${system};
        })
        systems);
  in {
    packages = forAllSystems (system: pkgs: let
      self = inputs.self.packages.${system};
    in {
      # The pretrained graph and its two sidecars. Upstream publishes them
      # only inside its Windows release binary, so that is what is fetched;
      # the script copies them out of its resource section without running it.
      gdi2m-model = pkgs.stdenvNoCC.mkDerivation {
        pname = "gdi2m-model";
        version = "0.1.0";
        src = pkgs.fetchurl {
          url = "https://github.com/maximmashkov1/gdi2m/releases/download/0.1.0/gdi2m_ui.exe";
          hash = "sha256-IxEl3FZQRvMuZpANyCg4KFcHKBCc44bcUebZaXgqA+M=";
        };
        dontUnpack = true;
        nativeBuildInputs = [pkgs.python3];
        installPhase = ''
          python3 ${./scripts/fetch_model.py} --exe $src --out $out/share/gdi2m
        '';
      };

      # The console tool from ./gdi2m, a vendored copy of the causal CNN+GRU
      # guitar transcriber (MIT) with a JACK backend added. The model sits
      # where the tool looks for it, so `gdi2m` runs as installed.
      gdi2m = pkgs.stdenv.mkDerivation {
        pname = "gdi2m";
        version = "0.1.0";
        src = ./gdi2m/tools/gdi2m;
        nativeBuildInputs = [pkgs.cmake pkgs.ninja pkgs.pkg-config];
        # libjack2 is headers and a link target only: at run time the loader
        # finds whichever libjack the system provides, PipeWire's included.
        buildInputs = [pkgs.onnxruntime pkgs.libjack2];
        cmakeFlags = ["-DCMAKE_BUILD_TYPE=Release"];
        postInstall = ''
          mkdir -p $out/share
          ln -s ${self.gdi2m-model}/share/gdi2m $out/share/gdi2m
        '';
        meta.mainProgram = "gdi2m";
      };
      default = self.gdi2m;
    });

    devShells = forAllSystems (system: pkgs: {
      default = pkgs.mkShell {
        packages = [
          inputs.self.packages.${system}.gdi2m
          # for bench/ and scripts/
          (pkgs.python3.withPackages (ps: [
            ps.numpy
            ps.soundfile
            # its matplotlib image-comparison tests fail in nixpkgs; we only use the matcher
            (ps.mir-eval.overridePythonAttrs (_: {doCheck = false;}))
          ]))
        ];
        # bench/ and scripts/ read the model's sidecar from here
        GDI2M_MODEL = "${inputs.self.packages.${system}.gdi2m-model}/share/gdi2m/gdi2m.onnx";
      };
    });
  };
}
