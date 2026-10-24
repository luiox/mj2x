// 语料：对象模型——三层继承链、super 调用、接口多实现分派、instanceof/checkcast、
// 字段遮蔽、初始化顺序（static → instance → 构造）、Object 契约（toString）。
public class ObjectsDispatch {
    static class Animal {
        String name = "animal";
        static int born = 0;

        Animal(String name) {
            born++;
            this.name = name;
            System.out.println("Animal<init>" + name);
        }

        String sound() { return "..."; }
        String describe() { return name + " says " + sound(); }
        static String census() { return "born=" + born; }
    }

    static class Dog extends Animal {
        String name = "dog-field";

        Dog(String n) {
            super(n);
            System.out.println("Dog<init>");
        }

        @Override String sound() { return "woof"; }
        @Override String describe() { return super.describe() + "!"; }
    }

    static class Puppy extends Dog {
        Puppy() {
            super("puppy");
            System.out.println("Puppy<init>");
        }

        @Override String sound() { return "yip"; }
    }

    interface Swimmer { String swim(); }
    interface Flyer { String fly(); }

    static class Duck extends Animal implements Swimmer, Flyer {
        Duck() { super("duck"); }
        @Override String sound() { return "quack"; }
        @Override public String swim() { return "paddle"; }
        @Override public String fly() { return "flap"; }
    }

    static class Shadow {
        int x = 1;
        static int s = 10;

        int read() { return x; }
        int readStatic() { return s; }
    }

    static class ShadowChild extends Shadow {
        int x = 2;   // 遮蔽父类字段（getfield 两张表）

        int readBoth() { return x + super.x; }
        static int bump() { return ++s; }
    }

    public static void main(String[] args) {
        System.out.println("-- init order --");
        Animal a = new Animal("generic");
        Dog d = new Dog("rex");
        Animal puppyAsAnimal = new Puppy();

        System.out.println("-- dispatch --");
        Animal[] zoo = {a, d, puppyAsAnimal, new Duck()};
        for (Animal animal : zoo) System.out.println(animal.describe());
        System.out.println("census " + Animal.census());

        Duck duck = new Duck();
        Swimmer swimmer = duck;
        Flyer flyer = duck;
        Animal asAnimal = duck;
        System.out.println("iface " + swimmer.swim() + " " + flyer.fly() + " "
                           + asAnimal.sound());

        // instanceof / checkcast
        for (Animal animal : zoo) {
            if (animal instanceof Dog) {
                Dog asDog = (Dog) animal;
                System.out.println("dog-like: " + asDog.sound());
            } else if (animal instanceof Flyer) {
                System.out.println("flyer: " + ((Flyer) animal).fly());
            } else {
                System.out.println("plain: " + animal.sound());
            }
        }
        Object o = d;
        if (o instanceof Puppy) System.out.println("puppy via object");
        System.out.println("not-dog " + (a instanceof Dog) + " null-inst " + (o == null));

        // 字段遮蔽：字段按静态类型解析，方法按动态分派
        ShadowChild sc = new ShadowChild();
        Shadow shadowView = sc;
        System.out.println("shadow " + sc.x + " " + shadowView.x + " " + sc.read() + " "
                           + sc.readBoth() + " " + sc.readStatic());
        System.out.println("static-bump " + ShadowChild.bump() + " " + Shadow.s);

        // toString（默认 Object.toString 含 identityHashCode 非确定，禁打印）
        System.out.println("tostring " + d.getClass().getName() + " " + duck.getClass().getName());
    }
}
