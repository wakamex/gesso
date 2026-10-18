// Reads lines "PUBLICKEY SIGNATURE MESSAGE" in hex (MESSAGE - for an empty one) and prints 1 or 0: ed25519-dalek's verify_strict.
use ed25519_dalek::{Signature, VerifyingKey};
use std::io::BufRead;
fn main() {
    for line in std::io::stdin().lock().lines() {
        let line = line.unwrap();
        let f: Vec<&str> = line.split_whitespace().collect();
        let pk: [u8; 32] = hex::decode(f[0]).unwrap().try_into().unwrap();
        let sig: [u8; 64] = hex::decode(f[1]).unwrap().try_into().unwrap();
        let msg = if f.len() > 2 && f[2] != "-" { hex::decode(f[2]).unwrap() } else { vec![] };
        let ok = VerifyingKey::from_bytes(&pk).map(|k| k.verify_strict(&msg, &Signature::from_bytes(&sig)).is_ok()).unwrap_or(false);
        println!("{}", ok as u8);
    }
}
