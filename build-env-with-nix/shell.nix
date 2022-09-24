with import <nixpkgs> { };

let
  gccNoCet = gcc12.cc.overrideAttrs (oldAttrs: rec {
    configureFlags = [ "--disable-cet" ] ++ oldAttrs.configureFlags;
  });


  oldPkgs = import
    (builtins.fetchTarball {
      url = "https://github.com/NixOS/nixpkgs/archive/05ae8b52071ff158a4d3c7036e13a2e932b2549b.tar.gz";
    })
    { };

  # 2.35.2 from nixos-21.11
  binutils-unwrapped-old = oldPkgs.binutils-unwrapped;

  gccNoCetWrap = wrapCCWith rec {
    cc = gccNoCet;
    bintools = wrapBintoolsWith {
      bintools = binutils-unwrapped-old;
    };
  };

in
(overrideCC stdenv gccNoCetWrap).mkDerivation
{
  name = "gccnocet";
  hardeningDisable = [ "all" ];

  buildInputs = [
  ];

  shellHook = ''
    export PS1="\n\[\033[1;32m\]etherboot-build:[\u \W]\$\[\033[0m\] "
    cd ../src/
    make
  '';
}
