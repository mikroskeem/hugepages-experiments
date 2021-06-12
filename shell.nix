{ pkgs ? import <nixpkgs> { } }:

pkgs.mkShell {
  nativeBuildInputs = with pkgs; [ gcc valgrind clang-analyzer ];

  CXXFLAGS = "-std=c++14";
}
