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
    packages = forAllSystems (system: pkgs: {
      # The console tool from ./gdi2m, a vendored copy of the causal CNN+GRU
      # guitar transcriber (MIT) with a JACK backend added.
      gdi2m = pkgs.stdenv.mkDerivation {
        pname = "gdi2m";
        version = "0.1.0";
        src = ./gdi2m/tools/gdi2m;
        nativeBuildInputs = [pkgs.cmake pkgs.ninja pkgs.pkg-config];
        # libjack2 is headers and a link target only: at run time the loader
        # finds whichever libjack the system provides, PipeWire's included.
        buildInputs = [pkgs.onnxruntime pkgs.libjack2];
        cmakeFlags = ["-DCMAKE_BUILD_TYPE=Release"];
        meta.mainProgram = "gdi2m";
      };
      default = inputs.self.packages.${system}.gdi2m;
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
        # The model is fetched, not built: python3 scripts/fetch_model.py
        shellHook = ''
          export GDI2M_MODEL="$(git rev-parse --show-toplevel 2>/dev/null || pwd)/models/gdi2m/gdi2m.onnx"
        '';
      };
    });
  };
}
