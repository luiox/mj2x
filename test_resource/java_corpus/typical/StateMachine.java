// 典型程型：枚举状态机——投币售货机（enum 状态 + 事件驱动的 switch 转移 +
// 非法事件捕获）。状态机是混淆后控制流分析最典型的对手面。
public class StateMachine {
    enum State { IDLE, HAS_COIN, DISPENSING, OUT_OF_ORDER }

    enum Event { INSERT_COIN, SELECT, TAKE, REFUND, MAINTENANCE }

    static class Machine {
        State state = State.IDLE;
        int stock = 2;
        int coins = 0;
        final StringBuilder log = new StringBuilder();

        void handle(Event event) {
            log.append(state).append('+').append(event).append("->");
            switch (state) {
                case IDLE:
                    switch (event) {
                        case INSERT_COIN: state = State.HAS_COIN; break;
                        case MAINTENANCE:
                            state = stock > 0 ? State.IDLE : State.OUT_OF_ORDER;
                            break;
                        default: reject();
                    }
                    break;
                case HAS_COIN:
                    switch (event) {
                        case SELECT:
                            if (stock == 0) { state = State.OUT_OF_ORDER; break; }
                            state = State.DISPENSING;
                            break;
                        case REFUND: state = State.IDLE; break;
                        case MAINTENANCE: state = State.OUT_OF_ORDER; break;
                        default: reject();
                    }
                    break;
                case DISPENSING:
                    switch (event) {
                        case TAKE:
                            stock--;
                            coins++;
                            state = stock > 0 ? State.IDLE : State.OUT_OF_ORDER;
                            break;
                        default: reject();
                    }
                    break;
                case OUT_OF_ORDER:
                    if (event == Event.MAINTENANCE && stock > 0) state = State.IDLE;
                    else reject();
                    break;
            }
            log.append(state).append(';');
        }

        void reject() {
            throw new IllegalStateException("bad event " + state);
        }

        void fill(int n) {
            stock += n;
            if (state == State.OUT_OF_ORDER) state = State.IDLE;
        }
    }

    public static void main(String[] args) {
        Machine m = new Machine();
        Event[] script = {Event.INSERT_COIN, Event.SELECT, Event.TAKE,
                          Event.INSERT_COIN, Event.INSERT_COIN,
                          Event.SELECT, Event.TAKE,
                          Event.SELECT,
                          Event.MAINTENANCE};
        for (Event e : script) {
            try {
                m.handle(e);
            } catch (IllegalStateException ex) {
                System.out.println("reject: " + ex.getMessage());
            }
        }
        System.out.println(m.log);
        System.out.println("stock=" + m.stock + " coins=" + m.coins + " state=" + m.state);

        Machine m2 = new Machine();
        m2.stock = 0;
        for (Event e : new Event[] {Event.INSERT_COIN, Event.SELECT, Event.MAINTENANCE}) {
            try {
                m2.handle(e);
            } catch (IllegalStateException ex) {
                System.out.println("m2-reject: " + ex.getMessage());
            }
        }
        m2.fill(3);
        m2.handle(Event.INSERT_COIN);
        m2.handle(Event.SELECT);
        m2.handle(Event.TAKE);
        System.out.println("m2 " + m2.state + " stock=" + m2.stock);
    }
}
