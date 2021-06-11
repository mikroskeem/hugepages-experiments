{ pkgs ? import <nixpkgs> { } }:

pkgs.mkShell {
  nativeBuildInputs = with pkgs; [ gcc valgrind ];

  CXXFLAGS = "-std=c++14";
}
